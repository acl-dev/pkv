#include "stdafx.h"
#include "db/map_store.h"
#include "db/mdb/mdb.h"
#include <algorithm>
#include <climits>
#include <cstdlib>
#include <iostream>
#include <new>
#include <random>
#include <thread>

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
using fields = std::map<std::string, std::string>;
using pages = std::map<uint64_t, std::string>;
static void require(bool ok, const char* message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}
class memory : public mdb {
public:
    memory() : mdb(8) {}
    size_t reads = 0;
    pages snapshot(const std::string& key) {
        pages out;
        require(dbvisit_pages(key, [&](uint64_t id, const std::string& bytes) {
            out.emplace(id, bytes); return true;
        }) == batch_status::ok, "page snapshot");
        return out;
    }
protected:
    lookup_status dblookup_page(const std::string& owner, uint64_t id,
        std::string& out) override {
        ++reads; return mdb::dblookup_page(owner, id, out);
    }
};
static std::shared_ptr<memory> database() {
    auto out = std::make_shared<memory>();
    require(out->open(""), "open"); return out;
}
static bool run(shared_db db, const std::string& key,
    const std::function<bool(map_store&)>& work, bool adaptive = false) {
    return db->transact_page(key, [&](page_transaction& tx) {
        map_store store(tx, key, map_kind::hash, adaptive);
        return store.open() && work(store) && store.finish();
    }) == db::batch_status::ok;
}
static void seed(shared_db db, const fields& expected, bool adaptive = false) {
    require(run(db, "hash", [&](map_store& store) {
        store.initialize(4102444800123LL);
        for (const auto& item : expected) {
            bool added;
            if (!store.put(item.first, item.second, false, added) || !added)
                return false;
        }
        return true;
    }, adaptive), "seed");
}
static std::vector<std::string> ordered(const fields& expected) {
    std::vector<std::string> out;
    for (const auto& item : expected) out.push_back(item.first);
    std::sort(out.begin(), out.end(), [](const std::string& a,
        const std::string& b) {
        return scan_bucket(a) != scan_bucket(b) ?
            scan_bucket(a) < scan_bucket(b) : a < b;
    });
    return out;
}
static std::vector<scan_page> scan(shared_db db) {
    std::vector<scan_page> out;
    require(run(db, "hash", [&](map_store& store) {
        scan_options options; options.count = 5;
        do {
            scan_page page;
            if (!store.scan(options, page)) return false;
            options.cursor = page.cursor; out.push_back(std::move(page));
        } while (options.cursor);
        return true;
    }), "scan");
    return out;
}
static void verify(shared_db db, const fields& expected,
    const std::string& name = "hash") {
    const auto keys = ordered(expected);
    require(run(db, name, [&](map_store& store) {
        if (store.metadata().elements != expected.size()) return false;
        bool found;
        std::string value;
        for (const auto& field : expected) {
            if (!store.probe_hash_field(field.first, value, found) || !found ||
                value != field.second) return false;
        }
        if (!store.probe_hash_field("missing:probe", value, found) || found)
            return false;
        fields actual;
        if (!store.visit([&](const std::string& key, const std::string& value) {
            return actual.emplace(key, value).second;
        }) || actual != expected) return false;
        for (size_t i = 0; i < keys.size(); ++i) {
            bool found; uint64_t rank, length, bound;
            std::string key, value;
            if (!store.select(i, key, found, &value) || !found ||
                key != keys[i] || value != expected.at(key) ||
                !store.rank(key, rank, found) || !found || rank != i ||
                !store.length(key, length, found) || !found ||
                length != value.size() ||
                !store.bound_rank(key, false, bound) || bound != i ||
                !store.bound_rank(key, true, bound) || bound != i + 1)
                return false;
        }
        for (bool reverse : {false, true}) {
            std::vector<std::string> actual_keys;
            if (!store.visit_rank(reverse && !keys.empty() ? keys.size() - 1 :
                0, keys.size(), reverse, [&](const std::string& key,
                    const std::string&) { actual_keys.push_back(key);
                    return true; })) return false;
            auto wanted = keys;
            if (reverse) std::reverse(wanted.begin(), wanted.end());
            if (actual_keys != wanted) return false;
            if (!keys.empty()) {
                const size_t first = keys.size() / 3;
                const size_t count = std::min(size_t(7), reverse ? first + 1 :
                    keys.size() - first);
                actual_keys.clear();
                if (!store.visit_rank(first, count, reverse,
                    [&](const std::string& key, const std::string&) {
                        actual_keys.push_back(key); return true;
                    })) return false;
                wanted.clear();
                for (size_t j = 0; j < count; ++j)
                    wanted.push_back(keys[reverse ? first - j : first + j]);
                if (actual_keys != wanted) return false;
            }
        }
        size_t stopped = 0;
        if (!store.visit([&](const std::string&, const std::string&) {
            ++stopped; return false;
        }, false) || stopped != (keys.empty() ? 0U : 1U)) return false;
        if (!keys.empty()) {
            stopped = 0;
            if (!store.visit_rank(keys.size() / 2, keys.size(), true,
                [&](const std::string&, const std::string&) {
                    ++stopped; return false;
                }, false) || stopped != 1) return false;
        }
        return true;
    }), "model/rank/range");
}
static void filtered_value_reads() {
    auto db = database();
    const std::string key(200, 'k');
    seed(db, {{key, std::string(20000, 'v')}, {"ok", "small"}});
    require(run(db, "hash", [](map_store& store) { return store.partition(); }),
        "filtered migration");
    const auto snapshot = db->snapshot("hash");
    uint64_t head = 0;
    for (const auto& page : snapshot) {
        if (page.second.compare(0, 3, "PMN") != 0) continue;
        map_node node;
        require(map_page_codec::decode_node(page.second, node) ==
            map_page_status::ok, "filtered node");
        if (node.value.head) head = node.value.head;
    }
    require(head != 0, "filtered blob head");
    auto bad = snapshot.at(head); bad[0] = '?';
    require(db->transact_page("hash", [&](page_transaction& tx) {
        return tx.put("hash", head, bad);
    }) == db::batch_status::ok, "filtered corrupt fixture");
    require(run(db, "hash", [&](map_store& store) {
        scan_options options; options.has_match = true; options.pattern = "ok";
        scan_page page;
        if (!store.scan(options, page) || page.values !=
            std::vector<std::string>{"ok", "small"}) return false;
        bool found, exceeded; std::string value = "old";
        return store.get_bounded(key, 64, value, found, exceeded) && found &&
            exceeded && value.empty();
    }), "filtered/bounded read touched excluded value");
    require(!run(db, "hash", [&](map_store& store) {
        bool found; std::string value;
        return store.get(key, value, found);
    }), "corrupt value accepted");
}
static void codec() {
    map_root root; root.bucketed = true; root.tree = 100;
    root.last_id = 100; root.elements = 3;
    map_directory directory;
    directory.buckets[7].tree = 1; directory.buckets[7].elements = 2;
    directory.buckets[19].tree = 2; directory.buckets[19].elements = 1;
    std::string bytes, header;
    require(map_page_codec::encode_root(root, header) == map_page_status::ok &&
        header.substr(0, 6) == std::string("PMR\3\1\1", 6), "root v3");
    map_root decoded;
    require(map_page_codec::decode_root(header, decoded) == map_page_status::ok
        && decoded.bucketed && decoded.tree == 100, "root v3 decode");
    require(map_page_codec::encode_directory(root, directory, bytes) ==
        map_page_status::ok && bytes.size() == 4112, "directory encode");
    map_directory out;
    budget = 0;
    const auto status = map_page_codec::decode_directory(bytes, root, out);
    budget = -1;
    require(status == map_page_status::ok && out.buckets[7].elements == 2,
        "directory decode allocates");
    const auto unchanged = out;
    for (size_t i = 0; i < bytes.size(); ++i) {
        auto bad = bytes.substr(0, i);
        require(map_page_codec::decode_directory(bad, root, out) !=
            map_page_status::ok && out.buckets[7].tree ==
                unchanged.buckets[7].tree, "truncated directory");
    }
    directory.buckets[19].tree = 1;
    std::string keep = "keep";
    require(map_page_codec::encode_directory(root, directory, keep) ==
        map_page_status::invalid && keep == "keep", "aliased bucket roots");
    directory.buckets[19].tree = 2;
    directory.buckets[19].elements = 2;
    require(map_page_codec::encode_directory(root, directory, keep) ==
        map_page_status::invalid && keep == "keep", "directory total");
}
static void group_codec() {
    map_root root;
    root.bucketed = root.grouped = true;
    root.tree = 100; root.last_id = 101; root.elements = 3;
    map_group group, decoded;
    group.buckets[3].tree = 1; group.buckets[3].elements = 2;
    group.buckets[7].tree = 2; group.buckets[7].elements = 1;
    std::string bytes, header;
    root.expires_at_ms = 4102444800123LL;
    require(map_page_codec::encode_root(root, header) == map_page_status::ok &&
        header.size() == 20 && header[3] == 5,
        "grouped root encode");
    map_root restored;
    require(map_page_codec::decode_root(header, restored) ==
        map_page_status::ok && restored.grouped && restored.bucketed &&
        restored.expires_at_ms == root.expires_at_ms &&
        restored.tree == root.tree && restored.last_id == root.last_id &&
        restored.elements == root.elements, "grouped root decode");
    require(map_page_codec::decode_root(header.substr(0, 19), restored) ==
        map_page_status::invalid, "truncated grouped root");
    require(map_page_codec::encode_group(root, 256, group, bytes) ==
        map_page_status::ok && bytes.size() == 16 && bytes[3] == 4,
        "sparse group encode");
    budget = 0;
    const auto status = map_page_codec::decode_group(bytes, root, 256, decoded);
    budget = -1;
    require(status == map_page_status::ok && decoded.buckets[3].tree == 1,
        "group decoder allocates");
    for (size_t i = 0; i < bytes.size(); ++i)
        require(map_page_codec::decode_group(bytes.substr(0, i), root, 256,
            decoded) != map_page_status::ok && decoded.buckets[3].tree == 1,
            "truncated group changed output");
    require(map_page_codec::decode_group(bytes + '\0', root, 256, decoded) ==
        map_page_status::invalid && decoded.buckets[3].tree == 1,
        "sparse group with trailing bytes changed output");
    require(map_page_codec::decode_group(bytes, root, 0, decoded) ==
        map_page_status::invalid, "group tag mismatch");
    auto large_group = group;
    auto large_root = root;
    large_group.buckets[3].elements = 256;
    large_root.elements = 257;
    std::string legacy;
    require(map_page_codec::encode_group(large_root, 256, large_group, legacy) ==
        map_page_status::ok && legacy.size() == 72 && legacy[3] == 3 &&
        map_page_codec::decode_group(legacy, large_root, 256, decoded) ==
        map_page_status::ok && decoded.buckets[3].elements == 256,
        "16-bit count group round trip");
    root.last_id = uint64_t(UINT16_MAX) + 1;
    require(map_page_codec::encode_root(root, header) == map_page_status::ok &&
        header.size() == 48 && header[3] == 3 &&
        map_page_codec::decode_root(header, restored) == map_page_status::ok &&
        restored.last_id == root.last_id && restored.grouped,
        "wide grouped root round trip");
    require(map_page_codec::decode_group(bytes, root, 256, decoded) ==
        map_page_status::ok, "compact group survives a growing page ID counter");
    require(map_page_codec::encode_group(root, 256, group, bytes) ==
        map_page_status::ok && bytes.size() == 272 && bytes[3] == 2 &&
        map_page_codec::decode_group(bytes, root, 256, decoded) ==
            map_page_status::ok, "wide group round trip");
    group.buckets[7].tree = 1;
    require(map_page_codec::encode_group(root, 256, group, bytes) ==
        map_page_status::invalid, "group duplicate tree");
    group.buckets[7].tree = 2; group.buckets[7].elements = 2;
    require(map_page_codec::encode_group(root, 256, group, bytes) ==
        map_page_status::invalid, "group count mismatch");
}
static void migration_and_model() {
    auto db = database();
    fields expected;
    for (unsigned i = 0; i < 512; ++i)
        expected.emplace("key:" + std::to_string(i), std::string(64, 'v'));
    expected[std::string(200, 'b')] = std::string(20000, 'x');
    expected[std::string("a\0b", 3)] = "binary";
    seed(db, expected);
    const auto before = db->snapshot("hash");
    const auto before_scan = scan(db);
    require(run(db, "hash", [](map_store& store) {
        return store.partition() && store.metadata().bucketed;
    }), "partition");
    const auto after = db->snapshot("hash");
    std::set<unsigned> groups;
    for (const auto& item : expected)
        groups.insert(scan_bucket(item.first) / 16);
    require(after.size() == before.size() + 1 + groups.size(),
        "index rebuild lost pages");
    for (const auto& page : before) {
        if (page.second.compare(0, 3, "PMB") == 0)
            require(after.at(page.first) == page.second, "blob rewritten");
    }
    const auto after_scan = scan(db);
    require(before_scan.size() == after_scan.size(), "scan page boundaries");
    for (size_t i = 0; i < before_scan.size(); ++i)
        require(before_scan[i].cursor == after_scan[i].cursor &&
            before_scan[i].values == after_scan[i].values, "scan order");
    verify(db, expected);
    std::mt19937 random(710128);
    for (unsigned i = 0; i < 200; ++i) {
        const std::string key = "key:" + std::to_string(random() % 700);
        if (i % 3) {
            const std::string value(i % 5 ? 13 : 20000, 'z');
            const bool existed = expected.count(key) != 0;
            require(run(db, "hash", [&](map_store& store) {
                bool added;
                return store.put(key, value, false, added) && added != existed;
            }), "partitioned put");
            expected[key] = value;
        } else {
            const bool existed = expected.erase(key) != 0;
            require(run(db, "hash", [&](map_store& store) {
                bool removed;
                return store.erase(key, removed) && removed == existed;
            }), "partitioned erase");
        }
    }
    verify(db, expected);
    require(db->transact_pages({"hash", "copy"}, [&](page_transaction& tx) {
        map_store source(tx, "hash"), target(tx, "copy"); bool copied;
        return source.open() && target.open() && source.clone_to(target,
            copied) && copied && target.finish() && source.finish();
    }) == db::batch_status::ok, "partitioned clone");
    verify(db, expected, "copy");
    require(run(db, "hash", [&](map_store& store) {
        for (const auto& item : expected) {
            bool removed;
            if (!store.erase(item.first, removed) || !removed) return false;
        }
        return !store.exists();
    }), "partitioned final erase");
    require(db->snapshot("hash").empty(), "empty hash retained pages");
}
static void legacy_directory() {
    auto db = database(); fields expected;
    for (unsigned i = 0; i < 300; ++i)
        expected["legacy:" + std::to_string(i)] = "value";
    seed(db, expected, true);
    std::string bytes;
    require(db->get("hash", bytes), "legacy root read");
    map_root root;
    require(map_page_codec::decode_root(bytes, root) == map_page_status::ok &&
        root.grouped, "legacy fixture root");
    const auto snapshot = db->snapshot("hash");
    map_group top;
    require(map_page_codec::decode_group(snapshot.at(root.tree), root, 256,
        top) == map_page_status::ok, "legacy fixture top");
    map_directory directory;
    for (unsigned i = 0; i < 16; ++i) {
        if (!top.buckets[i].tree) continue;
        auto state = root;
        state.tree = top.buckets[i].tree;
        state.elements = top.buckets[i].elements;
        map_group group;
        require(map_page_codec::decode_group(snapshot.at(state.tree), state,
            i, group) == map_page_status::ok, "legacy fixture group");
        for (unsigned j = 0; j < 16; ++j)
            directory.buckets[i * 16 + j] = group.buckets[j];
    }
    root.grouped = false;
    require(db->transact_page("hash", [&](page_transaction& tx) {
        for (const auto& entry : top.buckets)
            if (entry.tree && !tx.erase("hash", entry.tree)) return false;
        std::string encoded;
        if (map_page_codec::encode_directory(root, directory, encoded) !=
            map_page_status::ok || !tx.put("hash", root.tree,
                std::move(encoded))) return false;
        auto* entry = tx.roots().find("hash");
        if (!entry || map_page_codec::encode_root(root, entry->value) !=
            map_page_status::ok) return false;
        entry->action = db::update_action::write;
        return true;
    }) == db::batch_status::ok, "legacy directory publish");
    verify(db, expected);
    require(run(db, "hash", [](map_store& store) {
        bool added;
        return store.put("legacy:extra", "new", false, added) && added;
    }), "legacy directory update");
    expected["legacy:extra"] = "new";
    verify(db, expected);
}
static void rollback() {
    auto db = database(); fields expected;
    for (unsigned i = 0; i < 17; ++i) expected[std::to_string(i)] = "value";
    seed(db, expected);
    const auto pages_before = db->snapshot("hash");
    std::string before, after;
    require(db->get("hash", before), "rollback snapshot");
    bool success = false; unsigned failures = 0;
    for (long position = 0; position < 3000; ++position) {
        budget = position;
        bool ok = false;
        try { ok = run(db, "hash", [](map_store& store) {
            return store.partition(); }); }
        catch (const std::bad_alloc&) {}
        budget = -1;
        if (ok) { success = true; break; }
        ++failures;
        require(db->get("hash", after) && after == before &&
            db->snapshot("hash") == pages_before, "failed migration published");
    }
    require(success && failures > 0, "migration failure sweep");
    verify(db, expected);
    std::cout << "bucket migration allocation positions: " << failures << '\n';
}
static void adaptive_and_corruption() {
    auto db = database(); fields expected;
    for (unsigned i = 0; i < 129; ++i) expected[std::to_string(i)] = "v";
    seed(db, expected, true);
    require(run(db, "hash", [](map_store& store) {
        return store.metadata().bucketed;
    }), "adaptive threshold");
    auto snapshot = db->snapshot("hash");
    std::string root_bytes; require(db->get("hash", root_bytes), "root read");
    map_root root;
    require(map_page_codec::decode_root(root_bytes, root) ==
        map_page_status::ok, "root decode");
    auto bad = snapshot.at(root.tree); bad[6] = 1;
    require(db->transact_page("hash", [&](page_transaction& tx) {
        return tx.put("hash", root.tree, bad);
    }) == db::batch_status::ok, "corrupt directory fixture");
    require(db->transact_page("hash", [&](page_transaction& tx) {
        map_store store(tx, "hash"); bool found;
        std::string value = "keep";
        require(store.open() && !store.get("1", value, found) &&
            store.error() == map_store::status::invalid && value == "keep",
            "corrupt directory read");
        return true;
    }) == db::batch_status::aborted, "ignored directory error committed");
    require(db->transact_page("hash", [&](page_transaction& tx) {
        return tx.put("hash", root.tree, snapshot.at(root.tree));
    }) == db::batch_status::ok, "restore directory");
    verify(db, expected);
}
static void index_workspace_ownership() {
    using workspace = detail::map_index_workspace;
    workspace outer;
    budget = 0;
    auto* original = &outer.acquire();
    budget = -1;
    original->top.buckets[0].tree = 100;
    original->group_index = 7;
    {
        workspace nested;
        bool failed = false;
        budget = 0;
        try { nested.acquire(); } catch (const std::bad_alloc&) {
            failed = true;
        }
        budget = -1;
        require(failed && !nested && original->top.buckets[0].tree == 100,
            "failed nested index lease changed outer");
        auto& other = nested.acquire();
        other.top.buckets[0].tree = 200;
        require(&other != original && original->top.buckets[0].tree == 100,
            "nested index state aliases");
    }
    original->directory.reset(new map_directory);
    outer.reset();
    budget = 0;
    auto* reused = &outer.acquire();
    budget = -1;
    require(reused == original && !reused->directory &&
        reused->group_index == 16, "index lease retained heap/snapshot");
    std::vector<std::thread> workers;
    for (unsigned t = 0; t < 4; ++t) workers.emplace_back([t] {
        workspace own;
        auto& state = own.acquire();
        state.top.buckets[0].tree = t + 1;
        for (unsigned i = 0; i < 100; ++i) {
            workspace nested;
            nested.acquire().top.buckets[0].tree = 99;
            std::this_thread::yield();
            require(state.top.buckets[0].tree == t + 1,
                "index lease crossed workers");
        }
    });
    for (auto& worker : workers) worker.join();
}
static void index_workspace_fibers() {
    auto outer = database(), nested = database(), peer = database();
    auto prepare = [](shared_db db, unsigned count, const std::string& value) {
        fields values;
        for (unsigned i = 0; i < count; ++i)
            values[std::to_string(i)] = value;
        seed(db, values, true);
    };
    prepare(outer, 129, "outer"); prepare(nested, 300, "nested");
    prepare(peer, 500, "peer");
    auto read = [](map_store& store, const std::string& expected) {
        bool found; std::string out;
        return store.get("1", out, found) && found && out == expected;
    };
    bool entered = false, finished = false;
    go[&] {
        require(run(outer, "hash", [&](map_store& store) {
            if (!read(store, "outer") || !run(nested, "hash",
                [&](map_store& inner) { return read(inner, "nested"); }))
                return false;
            entered = true;
            while (!finished) acl::fiber::yield();
            return read(store, "outer");
        }), "yielding index owner");
    };
    go[&] {
        while (!entered) acl::fiber::yield();
        require(run(peer, "hash", [&](map_store& store) {
            return read(store, "peer");
        }), "index peer fiber");
        finished = true;
    };
    acl::fiber::schedule();
    require(entered && finished, "index fiber test did not interleave");
    require(run(outer, "hash", [&](map_store& store) {
        if (!read(store, "outer")) return false;
        budget = 0;
        bool ok = false;
        try { ok = read(store, "outer"); } catch (const std::bad_alloc&) {}
        budget = -1;
        return ok;
    }), "warm grouped Hash read allocates");
    std::string root_bytes;
    require(outer->get("hash", root_bytes), "index raw root");
    map_root root;
    require(map_page_codec::decode_root(root_bytes, root) ==
        map_page_status::ok, "index raw root decode");
    const auto snapshot = outer->snapshot("hash");
    require(outer->transact_page("hash", [&](page_transaction& tx) {
        map_store store(tx, "hash");
        if (!store.open() || !read(store, "outer")) return false;
        auto bad = snapshot.at(root.tree); bad[6] = 1;
        require(tx.put("hash", root.tree, bad), "index staged corruption");
        require(!read(store, "outer") && store.error() ==
            map_store::status::invalid, "index lease hid staged corruption");
        return true;
    }) == db::batch_status::aborted, "stale index read committed");
    std::cout << "warm grouped Hash: 0 allocations; index leases isolated\n";
}
static void scoped_group_invalidation() {
    auto db = std::make_shared<mdb>(8);
    require(db->open(""), "open scoped Hash");
    fields expected;
    for (unsigned i = 0; i < 150; ++i)
        expected.emplace(std::to_string(i), "value");
    seed(db, expected, true);
    const std::string key = "hash";
    execution_guard admission;
    db::page_lock_scope scope(*db, key);
    const auto read = [&](page_transaction& tx) {
        map_store store(tx, key, map_kind::hash);
        std::string bytes;
        bool found;
        return store.open() && store.get("42", bytes, found) && found &&
            bytes == "value" && store.finish();
    };
    require(scope.transact(*db, key, read) == db::batch_status::ok,
        "warm scoped Hash directory");
    const std::string replacement = "rollback:33";
    require(scan_bucket(replacement) == scan_bucket("42"), "rollback bucket");
    require(scope.transact(*db, key, [&](page_transaction& tx) {
        map_store store(tx, key, map_kind::hash);
        bool removed, added;
        require(store.open() && store.erase("42", removed) && removed &&
            store.put(replacement, "replacement", false, added) && added &&
            store.finish(), "stage same-bucket replacement");
        return false;
    }) == db::batch_status::aborted &&
        scope.transact(*db, key, read) == db::batch_status::ok,
        "aborted directory update escaped into the scope cache");
    require(scope.transact(*db, key, [&](page_transaction& tx) {
        map_root root;
        map_group top, group;
        std::string bytes;
        const auto bucket = scan_bucket("42");
        const auto group_index = bucket / 16;
        if (map_page_codec::decode_root(tx.roots().find(key)->value, root) !=
            map_page_status::ok ||
            tx.lookup(key, root.tree, bytes) != db::lookup_status::found ||
            map_page_codec::decode_group(bytes, root, 256, top) != map_page_status::ok)
            return false;
        auto group_root = root;
        group_root.tree = top.buckets[group_index].tree;
        group_root.elements = top.buckets[group_index].elements;
        if (tx.lookup(key, group_root.tree, bytes) != db::lookup_status::found ||
            map_page_codec::decode_group(bytes, group_root, group_index, group) !=
                map_page_status::ok) return false;
        for (unsigned i = 0; i < 16; ++i) {
            if (i == group_index || !top.buckets[i].elements) continue;
            top.buckets[i].tree = group.buckets[bucket % 16].tree;
            return map_page_codec::encode_group(root, 256, top, bytes) ==
                map_page_status::ok && tx.put(key, root.tree, bytes);
        }
        return false;
    }) == db::batch_status::ok, "alias a cached child tree from another parent");
    require(scope.transact(*db, key, read) == db::batch_status::aborted,
        "scope cache hid cross-group alias after parent replacement");
}

int main() {
    acl::acl_cpp_init();
    codec(); group_codec(); migration_and_model(); rollback();
    adaptive_and_corruption(); legacy_directory();
    filtered_value_reads(); index_workspace_ownership();
    index_workspace_fibers();
    scoped_group_invalidation();
    std::cout << "partitioned Hash compatibility and rollback passed\n";
}
