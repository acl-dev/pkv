#include "stdafx.h"
#include "db/list_store.h"
#include "db/mdb/mdb.h"
#include <cstdlib>
#include <deque>
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

static void require(bool ok, const char* reason) {
    if (!ok) {
        std::cerr << reason << '\n';
        std::exit(1);
    }
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
        for (const auto& page : pages) {
            largest = std::max(largest, page.value.size());
        }
        return mdb::dbapply_pages(roots, pages);
    }
};

static std::shared_ptr<tracking_db> memory() {
    auto out = std::make_shared<tracking_db>();
    require(out->open(""), "memory open");
    return out;
}

static bool operation(shared_db database, const std::string& key,
    const std::function<bool(list_store&)>& callback) {
    return database->transact_pages({key}, [&](page_transaction& view) {
        list_store list(view, key);
        return list.open() && callback(list) && list.finish();
    }) == db::batch_status::ok;
}

static std::vector<std::string> contents(shared_db database,
    const std::string& key) {
    std::vector<std::string> out;
    require(operation(database, key, [&](list_store& list) {
        return list.range(0, -1, out);
    }), "read list contents");
    return out;
}

static void test_packing_and_access() {
    auto database = memory();
    const std::string key("packed\0", 7);
    require(operation(database, key, [](list_store& list) {
        return list.push(std::vector<std::string>(10000, "x"), false);
    }), "packed seed");
    require(database->writes <= 20 && database->largest <= 16384,
        "small members were not packed into bounded pages");
    database->reads = database->writes = 0;
    require(operation(database, key, [](list_store& list) {
        require(list.metadata().elements == 10000, "metadata length");
        return true;
    }) && database->reads == 0, "length loaded page payloads");
    require(operation(database, key, [](list_store& list) {
        return list.push({"last"}, false);
    }) && database->reads == 1 && database->writes == 1,
        "tail append visited/rebuilt whole List");
    for (const auto index : {0LL, -1LL}) {
        database->reads = 0;
        require(operation(database, key, [&](list_store& list) {
            std::string value;
            bool found;
            return list.index(index, value, found) && found &&
                value == (index == 0 ? "x" : "last");
        }) && database->reads == 1, "endpoint index scanned whole List");
    }
    std::string root;
    require(database->get(key, root) && root.size() == 48 && root[3] == 2,
        "List root serialized members or omitted counter");
    database->reads = database->writes = 0;
    require(operation(database, key, [](list_store& list) {
        return list.set_expiry(4102444800123);
    }) && database->reads == 0 && database->writes == 0,
        "expiry update read/rewrote List payload pages");
    const auto all = contents(database, key);
    require(all.size() == 10001 && all.back() == "last", "packed range");
}

static void test_model() {
    auto database = memory();
    std::deque<std::string> model;
    std::mt19937 random(84123);
    const std::vector<std::string> values = {"", std::string("\0\xff", 2),
        "short", std::string(16328, 'a'), std::string(16329, 'b'),
        std::string(40000, 'c')};
    for (unsigned step = 0; step < 400; ++step) {
        const bool left = random() % 2;
        if (model.empty() || random() % 3 != 0) {
            std::vector<std::string> added;
            for (unsigned i = 0, count = 1 + random() % 3; i < count; ++i) {
                added.push_back(values[random() % values.size()]);
                if (left) model.push_front(added.back());
                else model.push_back(added.back());
            }
            require(operation(database, "model", [&](list_store& list) {
                return list.push(added, left);
            }), "model push");
        } else {
            const auto count = random() % 8;
            std::vector<std::string> expected;
            for (size_t i = 0, take = std::min(size_t(count), model.size());
                i < take; ++i) {
                expected.push_back(left ? model.front() : model.back());
                if (left) model.pop_front();
                else model.pop_back();
            }
            std::vector<std::string> popped;
            require(operation(database, "model", [&](list_store& list) {
                return list.pop(count, left, &popped);
            }) && popped == expected, "model pop order/value");
        }
        require(contents(database, "model") ==
            std::vector<std::string>(model.begin(), model.end()),
            "paged List differs from independent deque model");
        require(database->largest <= 16384, "oversized physical List value");
    }
    require(operation(database, "model", [](list_store& list) {
        return list.pop(UINT64_MAX, true, nullptr);
    }), "discard entire List");
    std::string value;
    require(!database->get("model", value), "empty List retained metadata");
}

static void test_large_and_reverse() {
    auto database = memory();
    std::string large(1024 * 1024, '\0');
    for (size_t i = 0; i < large.size(); ++i) large[i] = char(i % 251);
    for (const bool left : {false, true}) {
        require(operation(database, "large", [&](list_store& list) {
            list.initialize(4102444800123);
            return list.push({"before", large, "after"}, left);
        }), "fragmented push");
        require(operation(database, "large", [&](list_store& list) {
            std::vector<std::string> middle;
            bool found;
            std::string last;
            return list.metadata().expires_at_ms == 4102444800123 &&
                list.range(1, 1, middle) && middle ==
                    std::vector<std::string>{large} &&
                list.index(-1, last, found) && found &&
                last == (left ? "before" : "after");
        }), "fragmented range/index");
        std::vector<std::string> popped;
        require(operation(database, "large", [&](list_store& list) {
            return list.pop(3, left, &popped);
        }) && popped == std::vector<std::string>({"after", large, "before"}),
            "fragmented reverse pop");
    }
    require(database->largest <= 16384, "big member stored as big value");
}

static void test_search_and_trim() {
    auto database = memory();
    const std::vector<std::string> values = {"a", "b", "a", "c", "a"};
    require(operation(database, "search", [&](list_store& list) {
        return list.push(values, false);
    }), "search seed");
    for (const auto rank : {1LL, 2LL, -1LL, -2LL}) {
        require(operation(database, "search", [&](list_store& list) {
            list_search options;
            options.rank = rank;
            options.count = 0;
            std::vector<long long> out;
            const auto expected = rank == 1 ? std::vector<long long>{0, 2, 4} :
                rank == 2 ? std::vector<long long>{2, 4} : rank == -1 ?
                    std::vector<long long>{4, 2, 0} :
                    std::vector<long long>{2, 0};
            return list.positions("a", options, out) && out == expected;
        }), "ranked positions");
    }
    require(operation(database, "search", [](list_store& list) {
        return list.trim(1, -2);
    }) && contents(database, "search") ==
        std::vector<std::string>({"b", "a", "c"}), "two-ended trim");
    require(operation(database, "search", [](list_store& list) {
        return list.trim(99, 100);
    }) && contents(database, "search").empty(), "empty trim");
    require(operation(database, "stream-search", [](list_store& list) {
        return list.push({std::string(1024 * 1024, 'x'), "target"}, false);
    }), "streamed comparison seed");
    largest_allocation = 0;
    measure = true;
    const bool ok = operation(database, "stream-search", [](list_store& list) {
        list_search options;
        std::vector<long long> out;
        return list.positions("target", options, out) &&
            out == std::vector<long long>{1};
    });
    measure = false;
    require(ok && largest_allocation < 32768,
        "search materialized a complete large member");
}

static void test_reset_and_legacy_root() {
    auto database = memory();
    require(operation(database, "reset", [](list_store& list) {
        return list.push(std::vector<std::string>(30, std::string(1000, 'o')),
            false);
    }), "reset seed");
    require(operation(database, "reset", [](list_store& list) {
        if (!list.push({std::string(40000, 'd')}, false)) return false;
        list.initialize(4102444800123);
        return list.push({"new"}, false);
    }), "reset replace");
    require(database->transact_pages({"reset"}, [](page_transaction& view) {
        std::string bytes;
        require(view.lookup("reset", 2, bytes) == db::lookup_status::missing,
            "replace left orphan physical page");
        return true;
    }) == db::batch_status::ok, "reset page inspection");
    // Convert metadata to the existing 40-byte format, retaining actual pages.
    require(database->transact_pages({"reset"}, [](page_transaction& view) {
        auto& entry = *view.roots().find("reset");
        list_root root;
        require(list_page_codec::decode_root(entry.value, root) ==
            list_page_status::ok, "legacy root decode");
        root.last_id = 0;
        require(list_page_codec::encode_root(root, entry.value) ==
            list_page_status::ok, "legacy root encode");
        entry.action = db::update_action::write;
        return true;
    }) == db::batch_status::ok, "legacy root fixture");
    require(operation(database, "reset", [](list_store& list) {
        return list.push({std::string(20000, 'x')}, false);
    }), "legacy counter promotion");
    require(contents(database, "reset") == std::vector<std::string>({"new",
        std::string(20000, 'x')}), "promotion changed members");
}

static void test_corruption_and_exhaustion() {
    auto database = memory();
    require(operation(database, "corrupt", [](list_store& list) {
        return list.push({std::string(40000, 'x'), "end"}, false);
    }), "corruption seed");
    std::string original;
    require(database->transact_pages({"corrupt"}, [&](page_transaction& view) {
        return view.lookup("corrupt", 1, original) == db::lookup_status::found;
    }) == db::batch_status::ok, "corruption page read");
    for (const bool huge : {false, true}) {
        require(database->transact_pages({"corrupt"},
            [&](page_transaction& view) {
                list_page page;
                require(list_page_codec::decode_page(original, page) ==
                    list_page_status::ok, "corruption fixture decode");
                if (huge) page.fragments.front().total_bytes = UINT64_MAX;
                else page.next = 3;
                std::string encoded;
                return list_page_codec::encode_page(page, encoded) ==
                    list_page_status::ok && view.put("corrupt", 1, encoded);
            }) == db::batch_status::ok, "corruption fixture stage");
        largest_allocation = 0;
        measure = true;
        const bool ok = operation(database, "corrupt", [](list_store& list) {
            std::string value;
            bool found;
            return list.index(0, value, found);
        });
        measure = false;
        require(!ok && (!huge || largest_allocation < 32768),
            "corrupt chain/huge member accepted or allocated");
    }
    require(database->transact_pages({"corrupt"}, [&](page_transaction& view) {
        auto& entry = *view.roots().find("corrupt");
        list_root root;
        require(list_page_codec::decode_root(entry.value, root) ==
            list_page_status::ok, "exhaustion root decode");
        root.last_id = UINT64_MAX;
        entry.action = db::update_action::write;
        return view.put("corrupt", 1, original) &&
            list_page_codec::encode_root(root, entry.value) ==
                list_page_status::ok;
    }) == db::batch_status::ok, "counter exhaustion fixture");
    bool exhausted = false;
    require(!operation(database, "corrupt", [&](list_store& list) {
        const bool ok = list.push({std::string(16328, 'n')}, false);
        exhausted = list.error() == list_store::status::too_large;
        return ok;
    }) && exhausted, "page id counter wrapped");
    require(contents(database, "corrupt") == std::vector<std::string>({
        std::string(40000, 'x'), "end"}), "failed allocation changed List");
}

static void test_allocation_rollback() {
    bool succeeded = false;
    size_t failures = 0;
    for (long budget = 0; budget < 512; ++budget) {
        auto database = memory();
        const std::vector<std::string> original(30, std::string(1000, 'o'));
        require(operation(database, "fault", [&](list_store& list) {
            return list.push(original, false);
        }), "fault seed");
        std::string original_root;
        database->get("fault", original_root);
        std::vector<std::string> keys{"fault"};
        const db::page_callback callback = [](page_transaction& view) {
            list_store list(view, "fault");
            if (!list.open()) return false;
            list.initialize();
            return list.push({std::string(20000, 'n')}, false) && list.finish();
        };
        allocation_budget = budget;
        const auto result = database->transact_pages(std::move(keys), callback);
        allocation_budget = -1;
        if (result == db::batch_status::ok) {
            require(contents(database, "fault") ==
                std::vector<std::string>{std::string(20000, 'n')},
                "successful reset omitted data");
            succeeded = true;
            break;
        }
        ++failures;
        std::string root;
        require(database->get("fault", root) && root == original_root &&
            contents(database, "fault") == original,
            "failed reset exposed partial metadata/pages");
    }
    require(succeeded && failures > 0, "reset allocation sweep incomplete");
}

static void test_unfinished_context() {
    auto database = memory();
    require(operation(database, "unfinished", [](list_store& list) {
        return list.push({"base"}, false);
    }), "unfinished seed");
    require(database->transact_pages({"unfinished"},
        [](page_transaction& view) {
        list_store list(view, "unfinished");
        require(list.open() && list.push({"uncommitted"}, false),
            "unfinished stage");
        return true; // Missing finish must not commit page-only mutations.
    }) == db::batch_status::aborted && contents(database, "unfinished") ==
        std::vector<std::string>{"base"}, "unfinished context committed");
    require(database->transact_pages({"unfinished"},
        [](page_transaction& view) {
        view.roots().find("unfinished")->action = db::update_action::erase;
        list_store list(view, "unfinished");
        std::vector<std::string> popped;
        return list.open() && list.push({"temporary"}, true) &&
            list.pop(1, true, &popped) && popped ==
                std::vector<std::string>{"temporary"} &&
            list.push({"final"}, false) && list.finish();
    }) == db::batch_status::ok && contents(database, "unfinished") ==
        std::vector<std::string>{"final"}, "recreate after staged deletion");
    require(database->set("legacy", "legacy-codec-fixture"), "legacy seed");
    require(database->transact_pages({"legacy"}, [](page_transaction& view) {
        list_store list(view, "legacy");
        require(!list.open() && list.error() == list_store::status::not_paged,
            "legacy probe");
        list.initialize(); // Explicit replacement, not automatic conversion.
        return list.push({"converted"}, false) && list.finish();
    }) == db::batch_status::ok && contents(database, "legacy") ==
        std::vector<std::string>{"converted"}, "authorized legacy replacement");
}

static void test_yielding_transactions() {
    auto database = memory();
    require(operation(database, "shared", [](list_store& list) {
        return list.push({"base"}, false);
    }), "concurrency seed");
    std::vector<std::thread> workers;
    for (unsigned thread = 0; thread < 4; ++thread) {
        workers.emplace_back([&, thread] {
            for (unsigned fiber = 0; fiber < 4; ++fiber) go[&, thread, fiber] {
                for (unsigned i = 0; i < 20; ++i) {
                    const auto token = std::to_string(thread) + ":" +
                        std::to_string(fiber) + ":" + std::to_string(i);
                    require(operation(database, "shared", [&](list_store& list)
                        {
                        if (!list.push({token}, true)) return false;
                        acl::fiber::yield();
                        std::vector<std::string> out;
                        return list.pop(1, true, &out) && out ==
                            std::vector<std::string>{token};
                    }), "yielding read-own-pages transaction");
                }
            };
            acl::fiber::schedule();
        });
    }
    for (auto& worker : workers) worker.join();
    require(contents(database, "shared") == std::vector<std::string>{"base"},
        "concurrent endpoints lost original data");
}

int main() {
    acl::acl_cpp_init();
    test_packing_and_access();
    test_model();
    test_large_and_reverse();
    test_search_and_trim();
    test_reset_and_legacy_root();
    test_corruption_and_exhaustion();
    test_allocation_rollback();
    test_unfinished_context();
    test_yielding_transactions();
    std::cout << "Packed List page operations and atomic resets passed\n";
}
