#include "stdafx.h"
#include "db/map_store.h"
#include "db/list_store.h"
#include "db/mdb/mdb.h"
#include <cstdlib>
#include <iostream>
#include <new>
#include <thread>
#include <atomic>
#include <chrono>

static thread_local long budget = -1;
void* operator new(std::size_t n) {
    if (!budget) throw std::bad_alloc();
    if (budget > 0) --budget;
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
#if __cplusplus >= 201402L
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
#endif
using namespace pkv;
static void require(bool ok, const char* text) {
    if (!ok) { std::cerr << text << '\n'; std::exit(1); }
}
static bool run(shared_db database,
    const std::function<bool(map_store&)>& callback) {
    return database->transact_page("hash", [&](page_transaction& tx) {
        map_store map(tx, "hash");
        return map.open() && callback(map) && map.finish();
    }) == db::batch_status::ok;
}
static void seed(shared_db database, const std::string& value) {
    require(database->open(""), "open");
    require(run(database, [&](map_store& map) {
        bool added; return map.put("field", value, false, added) && added;
    }), "seed");
}
class inspecting : public mdb {
public:
    inspecting() : mdb(8) {}
    int mode = 0;
    size_t views = 0, borrowed = 0, copies = 0;
protected:
    bool dbborrows_pages() const override { return true; }
    lookup_status dblookup_page(const std::string& key, uint64_t id,
        std::string& out) override {
        ++copies; return mdb::dblookup_page(key, id, out);
    }
    lookup_status dbinspect_page(const std::string& key, uint64_t id,
        std::string& scratch, page_inspection& inspection) override {
        ++views;
        if (mode == 1) return lookup_status::found;
        if (mode == 4) {
            const std::string huge(page_transaction::max_value_bytes + 1, 'x');
            inspection.consume(huge); return lookup_status::found;
        }
        if (mode == 5) return lookup_status::error;
        struct state {
            page_inspection& inspection;
            const std::string* scratch; size_t* count;
        } request{inspection, &scratch, &borrowed};
        auto inspect = [](void* raw, const std::string& bytes) {
            auto& job = *static_cast<state*>(raw);
            require(&bytes != job.scratch, "native inspector copied bytes");
            ++*job.count; job.inspection.consume(bytes);
        };
        page_inspection counted(inspect, &request);
        const auto status = mdb::dbinspect_page(key, id, scratch, counted);
        if (mode == 2 && status == lookup_status::found)
            inspection.consume(scratch);
        return mode == 3 ? lookup_status::missing : status;
    }
};
class legacy : public mdb {
public:
    legacy() : mdb(8) {}
    size_t reads = 0;
    bool corrupt = false, reject = false;
protected:
    lookup_status dblookup_page(const std::string& key, uint64_t id,
        std::string& out) override {
        ++reads;
        if (reject) return lookup_status::error;
        const auto status = mdb::dblookup_page(key, id, out);
        if (corrupt && status == lookup_status::found) {
            if (out.compare(0, 4, "PMN\5", 4) == 0) out[6] = 0;
            else if (out.compare(0, 4, "PMN\4", 4) == 0) out[5] = char(255);
            else if (out.compare(0, 4, "PMN\3", 4) == 0 ||
                out.compare(0, 4, "PMN\11", 4) == 0) out[5] = 0;
            else out[5] = 1;
        }
        return status;
    }
};
static void test_protocol_and_owned_outputs() {
    auto database = std::make_shared<inspecting>();
    const std::string expected(64, 'v'); seed(database, expected);
    std::string saved;
    require(run(database, [&](map_store& map) {
        bool found; return map.get("field", saved, found) && found;
    }) && saved == expected && database->borrowed && !database->copies,
        "borrowed get");
    require(run(database, [&](map_store& map) {
        bool added; return map.put("field", "new", false, added) && !added;
    }) && saved == expected, "decoded output aliases backend bytes");
    for (int mode = 1; mode <= 5; ++mode) {
        database->mode = mode;
        require(database->transact_page("hash", [&](page_transaction& tx) {
            map_store map(tx, "hash"); bool found; std::string out = "keep";
            require(map.open() && !map.get("field", out, found) &&
                out == "keep" && map.error() == map_store::status::storage,
                "bad inspection contract accepted");
            return true;
        }) == db::batch_status::error, "inspection error committed");
    }
    database->mode = 0;
    require(run(database, [&](map_store& map) {
        bool added; return map.put("field", expected, false, added);
    }), "restore large value");
    require(database->transact_page("hash", [&](page_transaction& tx) {
        map_store map(tx, "hash"); bool found; std::string out = "keep";
        require(map.open(), "OOM open");
        budget = 0;
        bool ok = map.get("field", out, found);
        budget = -1;
        require(!ok && out == "keep", "borrowed decoder OOM accepted");
        return true;
    }) == db::batch_status::aborted, "decoder OOM committed");
    require(run(database, [&](map_store& map) {
        bool found; std::string out;
        return map.get("field", out, found) && found && out == expected;
    }), "borrowed lock not released after OOM");
}
static void test_legacy_hooks() {
    auto database = std::make_shared<legacy>(); seed(database, "value");
    const auto before = database->reads;
    require(run(database, [](map_store& map) {
        bool found; std::string out;
        return map.get("field", out, found) && found && out == "value";
    }) && database->reads == before + 1, "old lookup hook bypassed");
    for (bool storage : {false, true}) {
        database->corrupt = !storage; database->reject = storage;
        const auto status = database->transact_page("hash",
            [&](page_transaction& tx) {
                map_store map(tx, "hash"); bool found; std::string out = "keep";
                require(map.open() && !map.get("field", out, found) &&
                    out == "keep", "legacy hook result ignored");
                return true;
            });
        require(status == (storage ? db::batch_status::error :
            db::batch_status::aborted), "legacy hook failure committed");
    }
}
static void test_native_model() {
    shared_db database = std::make_shared<mdb>(8);
    require(database->open(""), "native open");
    std::map<std::string, std::string> expected;
    require(run(database, [&](map_store& map) {
        for (unsigned i = 0; i < 1000; ++i) {
            const auto key = std::to_string(i);
            const std::string value(i % 2 ? 64 : 12, 'a');
            bool added;
            if (!map.put(key, value, false, added) || !added) return false;
            expected[key] = value;
        }
        return true;
    }), "native seed");
    for (unsigned i = 0; i < 100; ++i) {
        const auto key = std::to_string(i * 7);
        require(run(database, [&](map_store& map) {
            bool changed;
            if (i % 2) return map.put(key, "changed", false, changed);
            return map.erase(key, changed) && changed;
        }), "native mutation");
        if (i % 2) expected[key] = "changed";
        else expected.erase(key);
    }
    require(run(database, [&](map_store& map) {
        std::map<std::string, std::string> actual;
        if (!map.visit([&](const std::string& key, const std::string& value) {
            actual[key] = value; return true;
        }) || actual != expected) return false;
        for (const auto& item : expected) {
            std::string value; bool found;
            if (!map.get(item.first, value, found) || !found ||
                value != item.second) return false;
        }
        return true;
    }), "native borrowed model");
}
static void test_native_concurrency() {
    shared_db database = std::make_shared<mdb>(1);
    require(database->open(""), "concurrent open");
    auto transact = [&](const std::string& owner,
        const std::function<bool(map_store&)>& callback) {
        return database->transact_page(owner, [&](page_transaction& tx) {
            map_store map(tx, owner);
            return map.open() && callback(map) && map.finish();
        }) == db::batch_status::ok;
    };
    for (unsigned t = 0; t < 4; ++t) for (unsigned f = 0; f < 4; ++f) {
        const auto owner = std::to_string(t) + ":" + std::to_string(f);
        require(transact(owner, [](map_store& map) {
            bool added;
            return map.put("field", std::string(64, 's'), false, added);
        }), "concurrent seed");
    }
    std::vector<std::thread> workers;
    for (unsigned t = 0; t < 4; ++t) workers.emplace_back([&, t] {
        for (unsigned f = 0; f < 4; ++f) go[&, t, f] {
            const auto owner = std::to_string(t) + ":" + std::to_string(f);
            std::string expected(64, 's');
            for (unsigned i = 0; i < 20; ++i) {
                const std::string next(64, static_cast<char>('a' + i % 26));
                require(transact(owner, [&](map_store& map) {
                    std::string out; bool found, added;
                    if (!map.get("field", out, found) || !found ||
                        out != expected || !map.put("field", next, false,
                            added) || added) return false;
                    acl::fiber::yield();
                    return out == expected && map.get("field", out, found)
                        && found && out == next;
                }), "concurrent borrowed/staged data crossed owners");
                expected = next;
            }
        };
        acl::fiber::schedule();
    });
    for (auto& worker : workers) worker.join();
}
static bool run_list(shared_db database,
    const std::function<bool(list_store&)>& callback) {
    return database->transact_page("list", [&](page_transaction& tx) {
        list_store list(tx, "list");
        return list.open() && callback(list) && list.finish();
    }) == db::batch_status::ok;
}
static void test_list_endpoints() {
    auto database = std::make_shared<inspecting>();
    require(database->open(""), "list open");
    const std::string value(64, 'a');
    require(run_list(database, [&](list_store& list) {
        return list.push({value, value}, false);
    }), "list seed");
    const auto before = database->borrowed;
    require(run_list(database, [&](list_store& list) {
        return list.push({"front"}, true);
    }) && database->borrowed > before && !database->copies,
        "list push copied input");
    std::vector<std::string> out;
    require(run_list(database, [&](list_store& list) {
        return list.pop(1, true, &out);
    }) && out == std::vector<std::string>{"front"}, "list borrowed pop");
    for (int mode = 1; mode <= 5; ++mode) {
        database->mode = mode;
        require(!run_list(database, [](list_store& list) {
            return list.push({"bad"}, false);
        }), "list bad inspection accepted");
    }
    database->mode = 0;
    require(database->transact_page("list", [&](page_transaction& tx) {
        list_store list(tx, "list");
        // Force an endpoint result larger than the recycled buffer limit.
        const std::vector<std::string> values{std::string(512, 'b')};
        require(list.open(), "list OOM open");
        budget = 0;
        const bool ok = list.push(values, true);
        budget = -1;
        require(!ok, "list endpoint OOM accepted");
        return true;
    }) == db::batch_status::aborted, "list OOM committed");
    const std::string large(30000, 'z');
    require(run_list(database, [&](list_store& list) {
        return list.push({large}, false);
    }), "list fragmented fallback");
    require(run_list(database, [&](list_store& list) {
        return list.pop(1, false, &out);
    }) && out == std::vector<std::string>{large}, "list fallback pop");
    require(run_list(database, [&](list_store& list) {
        return list.pop(2, false, &out);
    }) && out == std::vector<std::string>({value, value}),
        "list failed update changed contents");
    auto old = std::make_shared<legacy>();
    require(old->open("") && run_list(old, [&](list_store& list) {
        return list.push({value, value}, true);
    }), "legacy list seed");
    const auto reads = old->reads;
    require(run_list(old, [](list_store& list) {
        return list.push({"old"}, true);
    }) && old->reads == reads + 1, "legacy list hook bypassed");
    old->corrupt = true;
    require(!run_list(old, [](list_store& list) {
        return list.push({"bad"}, false);
    }), "legacy list corruption accepted");
}
static void test_native_list_concurrency() {
    auto database = std::make_shared<mdb>(1);
    require(database->open(""), "concurrent list open");
    std::vector<std::thread> workers;
    for (unsigned thread = 0; thread < 4; ++thread) {
        workers.emplace_back([&, thread] {
            for (unsigned fiber = 0; fiber < 4; ++fiber) {
                go[&, thread, fiber] {
                    const auto owner = "list" + std::to_string(thread * 4 +
                        fiber);
                    auto run = [&](const std::function<bool(list_store&)>& f) {
                        return database->transact_page(owner,
                            [&](page_transaction& tx) {
                                list_store list(tx, owner);
                                return list.open() && f(list) && list.finish();
                            }) == db::batch_status::ok;
                    };
                    require(run([](list_store& list) {
                        return list.push({"seed", "seed"}, true);
                    }), "concurrent list seed");
                    for (unsigned i = 0; i < 20; ++i) {
                        const bool left = i % 2 == 0;
                        const std::string value(64, char('a' + thread));
                        require(run([&](list_store& list) {
                            return list.push({value}, left);
                        }), "concurrent list push");
                        acl::fiber::yield();
                        std::vector<std::string> out;
                        require(run([&](list_store& list) {
                            if (!list.pop(1, left, &out)) return false;
                            acl::fiber::yield();
                            return out == std::vector<std::string>{value} &&
                                list.metadata().elements == 2;
                        }), "concurrent list pop output");
                    }
                };
            }
            acl::fiber::schedule();
        });
    }
    for (auto& worker : workers) worker.join();
}
class sibling_failure_db : public inspecting {
public:
    uint64_t rejected = 0;
    int failure = 0;
    std::vector<uint64_t> trace;
protected:
    lookup_status dbinspect_page(const std::string& owner, uint64_t id,
        std::string& scratch, page_inspection& inspection) override {
        trace.push_back(id);
        const auto previous = mode;
        mode = id == rejected && failure != 6 ? failure : 0;
        const auto result = inspecting::dbinspect_page(owner, id, scratch,
            inspection);
        mode = previous;
        return id == rejected && failure == 6 ? lookup_status::error : result;
    }
};
static void test_sibling_failure_order() {
    auto database = std::make_shared<sibling_failure_db>();
    require(database->open("") && run(database, [](map_store& map) {
        for (const auto* key : {"a", "b", "c"}) {
            bool added;
            if (!map.put(key, "value", false, added) || !added) return false;
        }
        return true;
    }), "sibling fixture");
    map_node root;
    require(database->transact_page("hash", [&](page_transaction& tx) {
        map_root metadata; std::string bytes;
        return map_page_codec::decode_root(tx.roots().begin()->value,
            metadata) == map_page_status::ok &&
            tx.lookup("hash", metadata.tree, bytes) ==
                db::lookup_status::found &&
            map_page_codec::decode_node(bytes, root) == map_page_status::ok;
    }) == db::batch_status::ok && root.left && root.right,
        "sibling root fixture");
    for (const auto id : {root.left, root.right})
    for (const int failure : {1, 2, 3, 5, 6}) {
        database->rejected = id; database->failure = failure;
        database->trace.clear();
        require(database->transact_pages({"hash", "peer"},
            [&](page_transaction& tx) {
            auto& peer = *tx.roots().find("peer");
            peer.value = "unpublished"; peer.action = db::update_action::write;
            require(tx.put("peer", 1, "unpublished"), "stage peer");
            map_store map(tx, "hash"); bool found; std::string out = "keep";
            require(map.open() && !map.get(root.key.inline_value, out,
                found) && !found && out == "keep" &&
                map.error() == map_store::status::storage,
                "sibling inspection failure ignored");
            return true;
        }) == db::batch_status::error, "sibling failure committed");
        const std::vector<uint64_t> expected = id == root.left ?
            std::vector<uint64_t>{root.id, root.left} :
            std::vector<uint64_t>{root.id, root.left, root.right};
        require(database->trace == expected, "sibling read order changed");
        std::string out;
        require(!database->get("peer", out), "sibling failure published peer");
    }
}
static void test_reader_lifetime() {
    auto database = std::make_shared<mdb>(1);
    require(database->open(""), "reader lifetime open");
    std::string peer = "peer";
    while ((std::hash<std::string>()(peer) & 255) ==
        (std::hash<std::string>()("hash") & 255)) peer += 'x';
    require(database->transact_page("reader-switch", [](page_transaction& tx) {
        map_store map(tx, "reader-switch"); bool added;
        return map.open() && map.put("a", "other", false, added) && added &&
            map.finish();
    }) == db::batch_status::ok, "reader second owner seed");
    for (unsigned generation = 0; generation < 8; ++generation) {
        const std::string value = "generation:" + std::to_string(generation);
        require(run(database, [&](map_store& map) {
            for (const auto* key : {"a", "b", "c"}) {
                bool added;
                if (!map.put(key, value, false, added) || !added) return false;
            }
            return true;
        }), "reader generation seed");
        std::atomic<unsigned> step{0};
        std::thread writer([&] {
            while (step.load() == 0) std::this_thread::yield();
            require(database->set(peer, value), "reader blocked peer writer");
            step.store(2);
        });
        require(run(database, [&](map_store& map) {
            std::string out; bool found;
            if (!map.get("a", out, found) || !found || out != value)
                return false;
            step.store(1);
            const auto deadline = std::chrono::steady_clock::now() +
                std::chrono::seconds(10);
            while (step.load() != 2) {
                require(std::chrono::steady_clock::now() < deadline,
                    "reader retained native lock across callback");
                std::this_thread::yield();
            }
            bool changed;
            return map.get("c", out, found) && found && out == value &&
                map.put("a", "staged", false, changed) && !changed &&
                map.get("a", out, found) && found && out == "staged";
        }), "reader borrowed/staged lifetime");
        writer.join();
        require(database->transact_pages({"hash", "reader-switch"},
            [&](page_transaction& tx) {
            map_store map(tx, "hash"); std::string out; bool found;
            map_store other(tx, "reader-switch");
            return map.open() && map.get("b", out, found) && found &&
                out == value && other.open() &&
                other.get("a", out, found) && found && out == "other" &&
                map.get("c", out, found) && found && out == value &&
                map.finish() && other.finish();
        }) == db::batch_status::ok, "reader multi-owner lifetime");
        require(database->del("hash"), "reader generation erase");
    }
}
static void test_typed_reader_contract() {
    std::string bytes("native page");
    auto read = [](const void* raw, uint64_t id) -> const std::string* {
        return id == 1 ? static_cast<const std::string*>(raw) : nullptr;
    };
    db::page_reader reader(read, &bytes);
    struct state {
        const std::string* expected;
        size_t calls;
        std::string output;
    } value{&bytes, 0, "unchanged"};
    auto consume = [](void* raw, const std::string& page) {
        auto& job = *static_cast<state*>(raw);
        require(&page == job.expected, "typed reader copied source bytes");
        ++job.calls;
        job.output = page;
    };
    require(reader.inspect(2, consume, &value) == db::lookup_status::missing &&
        value.calls == 0 && value.output == "unchanged", "typed missing contract");
    require(reader.inspect(1, consume, &value) == db::lookup_status::found &&
        value.calls == 1 && value.output == bytes, "typed single consumption");
    bytes.assign(db::page_inspection::max_bytes, 'x');
    require(reader.inspect(1, consume, &value) == db::lookup_status::found &&
        value.calls == 2 && value.output == bytes, "typed maximum-size page");
    bytes.push_back('x');
    const auto before = value.output;
    require(reader.inspect(1, consume, &value) == db::lookup_status::error &&
        value.calls == 2 && value.output == before, "typed oversized page consumed");
    db::page_inspection erased(consume, &value);
    const auto result = reader.inspect(1, erased);
    require(!erased.matches(result) && value.calls == 2,
        "erased oversized inspection differs");
    bytes.resize(db::page_inspection::max_bytes);
    std::string().swap(value.output);
    bool failed = false;
    budget = 0;
    try { reader.inspect(1, consume, &value); }
    catch (const std::bad_alloc&) { failed = true; }
    budget = -1;
    require(failed && value.calls == 3, "typed decoder allocation failure lost");
    require(reader.inspect(1, consume, &value) == db::lookup_status::found &&
        value.calls == 4 && value.output == bytes, "typed reader failed to recover");
}

int main() {
    acl::acl_cpp_init();
    test_protocol_and_owned_outputs(); test_legacy_hooks(); test_native_model();
    test_native_concurrency();
    test_list_endpoints();
    test_native_list_concurrency();
    test_sibling_failure_order();
    test_reader_lifetime();
    test_typed_reader_contract();
    std::cout <<
        "page inspection: native borrowing, legacy hooks, OOM passed\n";
}
