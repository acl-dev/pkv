#include "stdafx.h"
#include "db/list_store.h"
#include "db/mdb/mdb.h"
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <new>
#include <random>
#include <thread>

static thread_local long allocation_budget = -1;
static thread_local bool measure = false;
static thread_local size_t largest_allocation = 0;
void* operator new(std::size_t size) {
    if (allocation_budget == 0) throw std::bad_alloc();
    if (allocation_budget > 0) --allocation_budget;
    if (measure) largest_allocation = std::max(largest_allocation, size);
    if (void* pointer = std::malloc(size ? size : 1)) return pointer;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
#if __cplusplus >= 201402L
void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer);
    }
void operator delete[](void* pointer, std::size_t) noexcept {
    std::free(pointer);
}
#endif

using namespace pkv;
static const int64_t expiry = 4102444800123;

static void require(bool ok, const char* reason) {
    if (!ok) { std::cerr << reason << '\n'; std::exit(1); }
}

class tracking_db : public mdb {
public:
    tracking_db() : mdb(4) {}
    size_t reads = 0;
    size_t writes = 0;
    size_t largest = 0;
protected:
    lookup_status dblookup_page(const std::string& owner, uint64_t id,
        std::string& out) override {
        ++reads;
        return mdb::dblookup_page(owner, id, out);
    }
    batch_status dbapply_pages(const std::vector<batch_entry>& roots,
        const std::vector<page_change>& pages) override {
        writes += pages.size();
        for (const auto& page : pages) largest = std::max(largest,
            page.value.size());
        return mdb::dbapply_pages(roots, pages);
    }
};

static std::shared_ptr<tracking_db> memory() {
    auto database = std::make_shared<tracking_db>();
    require(database->open(""), "memory open");
    return database;
}

static bool operation(shared_db database, const std::string& key,
    const std::function<bool(list_store&)>& callback) {
    return database->transact_pages({key}, [&](page_transaction& view) {
        list_store list(view, key);
        return list.open() && callback(list) && list.finish();
    }) == db::batch_status::ok;
}

static void seed(shared_db database, const std::string& key,
    const std::vector<std::string>& values, int64_t deadline = expiry) {
    require(operation(database, key, [&](list_store& list) {
        list.initialize(deadline);
        return list.push(values, false);
    }), "List seed");
}

static std::vector<std::string> contents(shared_db database,
    const std::string& key, int64_t deadline = expiry) {
    std::vector<std::string> out;
    require(operation(database, key, [&](list_store& list) {
        require(!list.exists() || list.metadata().expires_at_ms == deadline,
            "mutation changed expiry");
        return list.range(0, -1, out);
    }), "read contents");
    return out;
}

static long long remove_model(std::vector<std::string>& values, long long count,
    const std::string& value) {
    long long removed = 0;
    if (count >= 0) {
        for (size_t i = 0; i < values.size();) {
            if (values[i] == value && (!count || removed < count)) {
                values.erase(values.begin() + i);
                ++removed;
            } else ++i;
        }
    } else {
        for (size_t i = values.size(); i && removed < -count;) {
            --i;
            if (values[i] == value) {
                values.erase(values.begin() + i);
                ++removed;
            }
        }
    }
    return removed;
}

static void test_edit_model() {
    auto database = memory();
    std::vector<std::string> model;
    const std::vector<std::string> pool = {"", "a", std::string("\0x\xff", 3),
        std::string(1000, 'm'), std::string(16328, 'p'),
        std::string(16329, 'q'), std::string(40000, 'r')};
    std::mt19937 random(98273);
    for (unsigned step = 0; step < 500; ++step) {
        const auto choice = random() % 4;
        const auto value = pool[random() % pool.size()];
        if (model.empty() || choice == 0) {
            const bool left = random() % 2;
            require(operation(database, "model", [&](list_store& list) {
                if (!list.exists()) list.initialize(expiry);
                return list.push({value}, left);
            }), "model push");
            model.insert(left ? model.begin() : model.end(), value);
        } else if (choice == 1) {
            const auto index = random() % model.size();
            const auto position = random() % 2 ? static_cast<long long>(index) :
                static_cast<long long>(index) -
                    static_cast<long long>(model.size());
            bool found = false;
            require(operation(database, "model", [&](list_store& list) {
                return list.replace(position, value, found);
            }) && found, "model replace");
            model[index] = value;
        } else if (choice == 2) {
            const bool before = random() % 2;
            const auto pivot = random() % 4 ? model[random() % model.size()] :
                "missing";
            auto at = std::find(model.begin(), model.end(), pivot);
            long long expected = -1;
            if (at != model.end()) {
                model.insert(before ? at : at + 1, value);
                expected = static_cast<long long>(model.size());
            }
            long long length;
            require(operation(database, "model", [&](list_store& list) {
                return list.insert(before, pivot, value, length);
            }) && length == expected, "model insert");
        } else {
            const auto count = static_cast<long long>(random() % 9) - 4;
            const auto expected = remove_model(model, count, value);
            long long removed;
            require(operation(database, "model", [&](list_store& list) {
                return list.remove(count, value, removed);
            }) && removed == expected, "model removal count");
        }
        require(contents(database, "model") == model,
            "middle edit differs from independent vector model");
        require(database->largest <= 16384, "middle edit stored big page");
    }
}

static void test_locality_and_large_values() {
    auto database = memory();
    seed(database, "local", std::vector<std::string>(10000, "x"));
    database->reads = database->writes = 0;
    bool found;
    require(operation(database, "local", [&](list_store& list) {
        return list.replace(5000, "y", found);
    }) && found && database->reads < 14 && database->writes == 1,
        "same-page replace rebuilt unrelated pages");
    database->writes = 0;
    long long length;
    require(operation(database, "local", [&](list_store& list) {
        return list.insert(true, "y", "z", length);
    }) && length == 10001 && database->writes <= 3,
        "split insertion rewrote unrelated List pages");
    const std::string big(1024 * 1024, 'b');
    seed(database, "big", {"before", big, "after"});
    largest_allocation = 0;
    measure = true;
    const bool replaced = operation(database, "big", [&](list_store& list) {
        return list.replace(1, "short", found);
    });
    measure = false;
    require(replaced && found && largest_allocation < 32768 &&
        contents(database, "big") ==
            std::vector<std::string>({"before", "short", "after"}),
        "large replacement materialized old member or changed neighbors");
    largest_allocation = 0;
    measure = true;
    const bool inserted = operation(database, "big", [&](list_store& list) {
        return list.insert(false, "before", big, length);
    });
    measure = false;
    require(inserted && length == 4 && largest_allocation < 32768,
        "large insertion allocated whole serialized member/List");
    require(contents(database, "big") == std::vector<std::string>({"before",
        big, "short", "after"}), "fragmented insertion order");
    long long removed;
    largest_allocation = 0;
    measure = true;
    const bool erased = operation(database, "big", [&](list_store& list) {
        return list.remove(0, big, removed);
    });
    measure = false;
    require(erased && removed == 1 && largest_allocation < 32768 &&
        contents(database, "big") ==
            std::vector<std::string>({"before", "short", "after"}),
        "streamed large removal allocated a whole member or lost neighbors");
    seed(database, "all", std::vector<std::string>(10000, "same"));
    database->writes = 0;
    require(operation(database, "all", [&](list_store& list) {
        return list.remove(0, "same", removed);
    }) && removed == 10000 && database->writes == 0,
        "all-match removal emitted every old page");
    std::string root;
    require(!database->get("all", root), "all-match removal retained root");
}

static void test_moves() {
    const std::string big(50000, 'x');
    for (const bool from_left : {false, true}) {
        for (const bool to_left : {false, true}) {
            auto database = memory();
            seed(database, "source", {"a", big, "c"});
            seed(database, "target", {"d", "e"}, expiry + 1000);
            std::vector<std::string> expected_source{"a", big, "c"};
            std::vector<std::string> expected_target{"d", "e"};
            for (unsigned step = 0; step < 3; ++step) {
                const auto expected = from_left ? expected_source.front() :
                    expected_source.back();
                expected_source.erase(from_left ? expected_source.begin() :
                    expected_source.end() - 1);
                expected_target.insert(to_left ? expected_target.begin() :
                    expected_target.end(), expected);
                std::string moved;
                bool found;
                require(database->transact_pages({"target", "source"},
                    [&](page_transaction& view) {
                        list_store source(view, "source"), target(view,
                            "target");
                        return source.open() && target.open() &&
                            source.move_to(target, from_left, to_left, moved,
                                found) && source.finish() && target.finish();
                    }) == db::batch_status::ok && found && moved == expected,
                    "cross-key move result");
                require(contents(database, "source") == expected_source &&
                    contents(database, "target", expiry + 1000) ==
                        expected_target, "cross-key move data/expiry");
            }
        }
    }
    auto database = memory();
    seed(database, "self", {"a", big, "c"});
    require(database->transact_pages({"self"}, [](page_transaction& view) {
        list_store source(view, "self");
        std::string value;
        bool found;
        return source.open() && source.move_to(source, true, false, value,
            found) && found && value == "a" && source.finish();
    }) == db::batch_status::ok && contents(database, "self") ==
        std::vector<std::string>({big, "c", "a"}), "same-key rotation");
}

static void test_clone() {
    auto database = memory();
    const std::string big(1024 * 1024, 'x');
    seed(database, "source", {"before", big, "after"});
    seed(database, "target", std::vector<std::string>(100,
        std::string(20000, 'o')));
    bool copied;
    largest_allocation = 0;
    measure = true;
    const auto result = database->transact_pages({"source", "target"},
        [&](page_transaction& view) {
            list_store source(view, "source"), target(view, "target");
            return source.open() && target.open() &&
                source.clone_to(target, copied) && source.finish() &&
                target.finish();
        });
    measure = false;
    require(result == db::batch_status::ok && copied &&
        largest_allocation < 32768 && contents(database, "target") ==
            contents(database, "source"), "bounded clone data/expiry");
    require(database->transact_pages({"target"}, [](page_transaction& view) {
        std::string value;
        return view.lookup("target", 100, value) == db::lookup_status::missing;
    }) == db::batch_status::ok, "clone retained destination orphan pages");
    bool found;
    require(operation(database, "target", [&](list_store& list) {
        return list.replace(1, "independent", found);
    }) && found && contents(database, "source") ==
        std::vector<std::string>({"before", big, "after"}),
        "clone created mutable page aliases");
}

static void test_edit_failures() {
    const std::vector<std::string> original = {"before",
        std::string(40000, 'o'), "after"};
    const std::vector<std::string> target_values = {"old-target",
        std::string(20000, 't')};
    const std::string replacement(60000, 'n');
    for (unsigned mode = 0; mode < 3; ++mode) {
        bool succeeded = false;
        size_t failures = 0;
        for (long budget = 0; budget < 512; ++budget) {
            auto database = memory();
            seed(database, "source", original);
            seed(database, "target", target_values, expiry + 1000);
            std::string root_before, target_before;
            database->get("source", root_before);
            database->get("target", target_before);
            std::vector<std::string> keys{"source", "target"};
            const db::page_callback callback = [&](page_transaction& view) {
                list_store source(view, "source"), target(view, "target");
                if (!source.open() || !target.open()) return false;
                bool done = false;
                std::string moved;
                const bool ok = mode == 0 ?
                    source.replace(1, replacement, done) : mode == 1 ?
                        source.move_to(target, true, false, moved, done) :
                        source.clone_to(target, done);
                return ok && done && source.finish() && target.finish();
            };
            allocation_budget = budget;
            const auto result = database->transact_pages(std::move(keys),
                callback);
            allocation_budget = -1;
            if (result == db::batch_status::ok) {
                const auto expected = mode == 0 ?
                    std::vector<std::string>{"before", replacement, "after"} :
                    mode == 1 ? std::vector<std::string>{original[1], "after"}
                        : original;
                const auto expected_target = mode == 1 ?
                    std::vector<std::string>{target_values[0], target_values[1],
                        "before"} : mode == 2 ? original : target_values;
                require(contents(database, "source") == expected &&
                    contents(database, "target", mode == 2 ? expiry :
                        expiry + 1000) == expected_target,
                    "successful injected edit omitted data");
                succeeded = true;
                break;
            }
            ++failures;
            std::string root_after, target_after;
            require(database->get("source", root_after) &&
                root_after == root_before &&
                database->get("target", target_after) &&
                target_after == target_before &&
                contents(database, "source") == original &&
                contents(database, "target", expiry + 1000) == target_values,
                "failed edit/move/clone changed metadata/pages");
        }
        require(succeeded && failures > 0, "allocation sweep incomplete");
    }
}

static void test_move_failure_after_pop() {
    auto database = memory();
    const std::string big(40000, 'x');
    seed(database, "source", {big});
    seed(database, "target", {"existing"});
    require(database->transact_pages({"target"}, [](page_transaction& view) {
        auto& root = *view.roots().find("target");
        list_root metadata;
        if (list_page_codec::decode_root(root.value, metadata) !=
            list_page_status::ok) return false;
        metadata.last_id = UINT64_MAX;
        root.action = db::update_action::write;
        return list_page_codec::encode_root(metadata, root.value) ==
            list_page_status::ok;
    }) == db::batch_status::ok, "exhausted destination seed");
    require(database->transact_pages({"source", "target"},
        [](page_transaction& view) {
            list_store source(view, "source"), target(view, "target");
            require(source.open() && target.open(), "move failure open");
            std::string value = "sentinel";
            bool found;
            require(!source.move_to(target, true, false, value, found) &&
                !found && value == "sentinel", "failed move returned a value");
            source.finish();
            target.finish();
            return true; // Ignoring the error must not commit the source pop.
        }) == db::batch_status::aborted &&
        contents(database, "source") == std::vector<std::string>{big} &&
        contents(database, "target") == std::vector<std::string>{"existing"},
        "destination failure committed source removal");
}

static db::batch_status copy_named(shared_db source, const std::string& from,
    shared_db target, const std::string& to, bool& copied) {
    return source->copy_pages_to(from, *target, to,
        [&](page_transaction& input, page_transaction& output) {
            list_store original(input, from), destination(output, to);
            if (!original.open()) return false;
            if (!original.exists()) { copied = false; return true; }
            return destination.open() && original.clone_to(destination,
                copied) && original.finish() && destination.finish();
        });
}

static void test_cross_database_copy() {
    auto left = db::create_mdb(4), right = db::create_mdb(4);
    require(left->open("") && right->open(""), "cross DB open");
    const std::vector<std::string> a{"left", std::string(20000, 'a')};
    const std::vector<std::string> b{"right", std::string(30000, 'b')};
    seed(left, "list", a);
    seed(right, "list", b, expiry + 1000);
    bool copied;
    require(copy_named(left, "list", left, "same-db", copied) ==
        db::batch_status::ok && copied && contents(left, "same-db") == a,
        "same-DB read-only page copy");
    std::vector<std::thread> workers;
    for (unsigned thread = 0; thread < 4; ++thread) {
        workers.emplace_back([&, thread] {
            for (unsigned i = 0; i < 20; ++i) {
                const auto& from = thread % 2 ? left : right;
                const auto& to = thread % 2 ? right : left;
                bool done;
                require(copy_named(from, "list", to, "copy", done) ==
                    db::batch_status::ok && done,
                    "opposed cross-DB copy failed/deadlocked");
            }
        });
    }
    for (auto& worker : workers) worker.join();
    require(contents(right, "copy") == a &&
        contents(left, "copy", expiry + 1000) == b &&
        contents(left, "list") == a &&
        contents(right, "list", expiry + 1000) == b,
        "cross-DB copy lost data/expiry or changed its source");
    require(copy_named(left, "missing", right, "copy", copied) ==
        db::batch_status::ok && !copied && contents(right, "copy") == a,
        "missing-source copy changed target");
    // Both directions now contend for the identical pair of DB/key locks.
    workers.clear();
    for (unsigned thread = 0; thread < 4; ++thread) {
        workers.emplace_back([&, thread] {
            for (unsigned i = 0; i < 20; ++i) {
                const auto& from = thread % 2 ? left : right;
                const auto& to = thread % 2 ? right : left;
                bool done;
                std::this_thread::yield();
                require(copy_named(from, "list", to, "list", done) ==
                    db::batch_status::ok && done,
                    "same-key opposed copy deadlocked/failed");
            }
        });
    }
    for (auto& worker : workers) worker.join();
    std::string encoded;
    list_root metadata;
    require(left->get("list", encoded) &&
        list_page_codec::decode_root(encoded, metadata) == list_page_status::ok,
        "same-key copy metadata");
    const auto deadline = metadata.expires_at_ms;
    const auto expected = deadline == expiry ? a : b;
    require((deadline == expiry || deadline == expiry + 1000) &&
        contents(left, "list", deadline) == expected &&
        contents(right, "list", deadline) == expected,
        "opposed copies mixed source values/expiry");
}

static void test_copy_source_guard() {
    for (const bool same : {false, true}) {
        auto source = memory();
        auto target = same ? source : memory();
        seed(source, "source", {"original"});
        seed(target, "target", {"destination"});
        for (unsigned mode = 0; mode < 3; ++mode) {
            require(source->copy_pages_to("source", *target, "target",
                [&](page_transaction& from, page_transaction& to) {
                    auto& original = *from.roots().find("source");
                    if (mode == 0) original.action = db::update_action::erase;
                    if (mode == 1) original.value = "unauthorized";
                    if (mode == 2) from.put("source", 1, "unauthorized");
                    list_store destination(to, "target");
                    destination.initialize();
                    return destination.push({"changed"}, false) &&
                        destination.finish();
                }) == db::batch_status::aborted &&
                contents(source, "source") ==
                    std::vector<std::string>{"original"} &&
                contents(target, "target") ==
                    std::vector<std::string>{"destination"},
                "copy callback changed source or partly committed target");
        }
    }
}

static void test_cross_copy_failures() {
    bool succeeded = false;
    size_t failures = 0;
    const std::vector<std::string> original{"source", std::string(40000, 's')};
    const std::vector<std::string> previous{"target", std::string(20000, 't')};
    for (long budget = 0; budget < 512; ++budget) {
        auto source = memory(), target = memory();
        seed(source, "key", original);
        seed(target, "key", previous, expiry + 1000);
        std::string source_root, target_root;
        source->get("key", source_root);
        target->get("key", target_root);
        const std::string key = "key";
        const db::page_copy_callback callback = [](page_transaction& from,
            page_transaction& to) {
            list_store original_list(from, "key"), copy(to, "key");
            bool copied;
            return original_list.open() && copy.open() &&
                original_list.clone_to(copy, copied) && copied &&
                original_list.finish() && copy.finish();
        };
        allocation_budget = budget;
        const auto result = source->copy_pages_to(key, *target, key, callback);
        allocation_budget = -1;
        if (result == db::batch_status::ok) {
            require(contents(target, key) == original &&
                contents(source, key) == original, "cross-copy success data");
            succeeded = true;
            break;
        }
        ++failures;
        std::string after_source, after_target;
        require(source->get(key, after_source) && after_source == source_root &&
            target->get(key, after_target) && after_target == target_root &&
            contents(source, key) == original &&
            contents(target, key, expiry + 1000) == previous,
            "failed cross-copy exposed partial data or metadata");
    }
    require(succeeded && failures > 0,
        "cross-copy allocation sweep incomplete");
}

int main() {
    acl::acl_cpp_init();
    test_edit_model();
    test_locality_and_large_values();
    test_moves();
    test_clone();
    test_edit_failures();
    test_move_failure_after_pop();
    test_cross_database_copy();
    test_copy_source_guard();
    test_cross_copy_failures();
    std::cout << "Bounded List middle edits, moves and cloning passed\n";
}
