#include "stdafx.h"
#include "db/list_store.h"
#include "db/mdb/mdb.h"
#include <algorithm>
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

static void test_small_push_packing(size_t width, size_t max_page = 2048) {
    for (bool left : {false, true}) {
        auto database = memory();
        std::deque<std::string> expected;
        for (unsigned i = 0; i < 2000; ++i) {
            const std::string value(width, static_cast<char>(i));
            require(operation(database, "small", [&](list_store& list) {
                return list.push({value}, left);
            }), "small packed push");
            if (left) expected.push_front(value);
            else expected.push_back(value);
        }
        require(database->largest <= max_page && contents(database, "small") ==
            std::vector<std::string>(expected.begin(), expected.end()),
            "small push packing changed bound/order");
    }
}

static void test_medium_push_packing() {
    for (size_t width : {size_t(512), size_t(1024), size_t(2048),
            size_t(3072)}) {
        for (bool left : {false, true}) {
            auto database = memory();
            std::deque<std::string> model;
            for (unsigned i = 0; i < 120; ++i) {
                const std::string value(width, static_cast<char>(i));
                require(operation(database, "medium", [&](list_store& list) {
                    return list.push({value}, left);
                }), "medium push");
                if (left) model.push_front(value);
                else model.push_back(value);
            }
            require(contents(database, "medium") ==
                std::vector<std::string>(model.begin(), model.end()),
                "medium packing order");
            const auto inspect = [&](page_transaction& tx) {
                list_root root;
                const auto* entry = tx.roots().find("medium");
                if (!entry || list_page_codec::decode_root(entry->value,
                    root) != list_page_status::ok) return false;
                uint64_t id = root.head, previous = 0;
                size_t members = 0, pages = 0;
                while (id) {
                    std::string bytes; list_page page;
                    if (++pages > 120 || tx.lookup("medium", id, bytes) !=
                        db::lookup_status::found || bytes.size() < 4 ||
                        bytes[3] != 2 ||
                        list_page_codec::decode_page(bytes, page) !=
                        list_page_status::ok || page.id != id ||
                        page.previous != previous) return false;
                    for (const auto& fragment : page.fragments) {
                        if (fragment.offset || fragment.bytes.size() != width
                            || fragment.total_bytes != width) return false;
                        ++members;
                    }
                    previous = id; id = page.next;
                }
                return previous == root.tail && members == 120;
            };
            require(database->transact_page("medium", inspect) ==
                db::batch_status::ok, "medium complete page chain");
            std::vector<std::string> popped;
            require(operation(database, "medium", [&](list_store& list) {
                return list.pop(117, left, &popped);
            }), "medium cross-page pop");
            std::vector<std::string> expected;
            for (unsigned i = 0; i < 117; ++i) {
                expected.push_back(left ? model.front() : model.back());
                if (left) model.pop_front(); else model.pop_back();
            }
            require(popped == expected && contents(database, "medium") ==
                std::vector<std::string>(model.begin(), model.end()),
                "medium pop order/tail");
        }
    }
}

static void test_medium_page_growth_rollback() {
    for (bool left : {false, true}) {
        bool succeeded = false;
        size_t failures = 0;
        const std::vector<std::string> original(4, std::string(1024, 'o'));
        const std::string added(1024, 'n');
        auto expected = original;
        expected.insert(left ? expected.begin() : expected.end(), added);
        for (long budget = 0; budget < 128; ++budget) {
            auto database = memory();
            require(operation(database, "medium-fault", [&](list_store& list) {
                return list.push(original, left);
            }), "medium growth rollback seed");
            std::string root;
            require(database->get("medium-fault", root), "medium fault root");
            std::vector<std::string> keys{"medium-fault"};
            const db::page_callback callback = [&](page_transaction& tx) {
                list_store list(tx, "medium-fault");
                return list.open() && list.push({added}, left) && list.finish();
            };
            allocation_budget = budget;
            const auto result = database->transact_pages(std::move(keys),
                callback);
            allocation_budget = -1;
            if (result == db::batch_status::ok) {
                require(contents(database, "medium-fault") == expected,
                    "medium growth successful retry");
                succeeded = true; break;
            }
            std::string after;
            require(database->get("medium-fault", after) && after == root &&
                contents(database, "medium-fault") == original,
                "medium growth failure exposed partial update");
            ++failures;
        }
        require(succeeded && failures, "medium growth fault sweep incomplete");
    }
}

static void test_old_medium_page_append() {
    for (bool left : {false, true}) {
        auto database = memory();
        const std::vector<std::string> original(14, std::string(1024, 'o'));
        require(operation(database, "wide-medium", [&](list_store& list) {
            return list.push(original, left);
        }), "old medium bulk page");
        database->reads = database->writes = 0;
        const std::string added(1024, 'n');
        require(operation(database, "wide-medium", [&](list_store& list) {
            return list.push({added}, left);
        }) && database->reads == 1 && database->writes == 1,
            "old medium page lost remaining capacity");
        auto expected = original;
        expected.insert(left ? expected.begin() : expected.end(), added);
        require(contents(database, "wide-medium") == expected,
            "old medium page append order");
    }
}

static void test_wide_page_append() {
    auto database = memory();
    require(database->transact_pages({"wide"}, [](page_transaction& tx) {
        list_fragment value;
        value.total_bytes = 3; value.bytes = "old";
        list_page page;
        page.id = 1; page.fragments.assign(600, value);
        list_root root;
        root.elements = 600; root.head = root.tail = root.last_id = 1;
        auto* entry = tx.roots().find("wide");
        entry->action = db::update_action::write;
        std::string bytes;
        return list_page_codec::encode_root(root, entry->value) ==
            list_page_status::ok && list_page_codec::encode_page(page,
                bytes) == list_page_status::ok && tx.put("wide", 1,
                    std::move(bytes));
    }) == db::batch_status::ok, "wide page fixture");
    database->reads = database->writes = 0;
    require(operation(database, "wide", [](list_store& list) {
        return list.push({"new"}, false);
    }) && database->reads == 1 && database->writes == 1,
        "wide page append lost free capacity");
    std::vector<std::string> expected(600, "old");
    expected.push_back("new");
    require(contents(database, "wide") == expected, "wide page append order");
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

static void test_pop_cycle_rollback() {
    auto database = memory();
    const std::string key = "cyclic-pop";
    require(operation(database, key, [](list_store& list) {
        return list.push({std::string(60000, 'x'), "end"}, false);
    }), "cyclic pop seed");
    std::string original;
    require(database->transact_page(key, [&](page_transaction& tx) {
        std::string bytes;
        list_page page;
        if (tx.lookup(key, 1, original) != db::lookup_status::found ||
            tx.lookup(key, 3, bytes) != db::lookup_status::found ||
            list_page_codec::decode_page(bytes, page) != list_page_status::ok ||
            !page.next) return false;
        page.next = 1;
        return list_page_codec::encode_page(page, bytes) == list_page_status::ok &&
            tx.put(key, 3, std::move(bytes));
    }) == db::batch_status::ok, "cyclic pop fixture");
    require(!operation(database, key, [](list_store& list) {
        std::string value;
        return list.pop_one(true, value);
    }), "cyclic pop accepted");
    require(database->transact_page(key, [&](page_transaction& tx) {
        std::string bytes;
        return tx.lookup(key, 1, bytes) == db::lookup_status::found &&
            bytes == original;
    }) == db::batch_status::ok, "cyclic pop published staged erasure");
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

static void test_endpoint_allocation_rollback() {
    for (bool left : {false, true}) {
        for (bool pop : {false, true}) {
            bool succeeded = false;
            size_t failures = 0;
            for (long budget = 0; budget < 256; ++budget) {
                auto database = memory();
                const std::vector<std::string> original(100,
                    std::string(30, 'o'));
                require(operation(database, "fault", [&](list_store& list) {
                    return list.push(original, false);
                }), "endpoint fault seed");
                auto expected = original;
                const std::vector<std::string> added{std::string(30, 'n')};
                if (pop) expected.pop_back();
                else expected.insert(left ? expected.begin() : expected.end(),
                    added.front());
                std::string original_root;
                database->get("fault", original_root);
                std::vector<std::string> keys{"fault"}, output;
                const db::page_callback callback = [&](page_transaction& tx) {
                    list_store list(tx, "fault");
                    if (!list.open()) return false;
                    return (pop ? list.pop(1, left, &output) :
                        list.push(added, left)) && list.finish();
                };
                allocation_budget = budget;
                const auto result = database->transact_pages(std::move(keys),
                    callback);
                allocation_budget = -1;
                if (result == db::batch_status::ok) {
                    require(contents(database, "fault") == expected &&
                        (!pop || output ==
                            std::vector<std::string>{original.front()}),
                        "successful endpoint operation lost data");
                    succeeded = true;
                    break;
                }
                ++failures;
                std::string root;
                require(database->get("fault", root) &&
                    root == original_root &&
                    contents(database, "fault") == original,
                    "failed endpoint operation exposed page/root mutation");
            }
            require(succeeded && failures > 0,
                "endpoint allocation sweep incomplete");
        }
    }
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

static void test_whole_member_pages(size_t width, unsigned count) {
    for (bool left : {false, true}) {
        auto database = memory();
        std::deque<std::string> expected;
        for (unsigned i = 0; i < count; ++i) {
            std::string value(width + (width == 64 ? i % 2 : 0),
                static_cast<char>(i));
            value[0] = '\0'; value[1] = static_cast<char>(i >> 8);
            require(operation(database, "whole", [&](list_store& list) {
                return list.push({value}, left);
            }), "whole member push");
            if (left) expected.push_front(value);
            else expected.push_back(value);
        }
        require(contents(database, "whole") ==
            std::vector<std::string>(expected.begin(), expected.end()),
            "whole member boundary changed binary ordering");
        require(database->transact_page("whole", [&](page_transaction& tx) {
            list_root root;
            auto* entry = tx.roots().find("whole");
            if (!entry || list_page_codec::decode_root(entry->value, root) !=
                list_page_status::ok) return false;
            uint64_t previous = 0, id = root.head;
            size_t members = 0, pages = 0;
            while (id) {
                std::string bytes;
                list_page page;
                if (++pages > 100 || tx.lookup("whole", id, bytes) !=
                    db::lookup_status::found || !list_page_codec::is_compact_page(bytes) || list_page_codec::decode_page(bytes,
                        page) != list_page_status::ok || page.id != id ||
                    page.previous != previous) return false;
                for (const auto& item : page.fragments) {
                    if (item.offset || item.total_bytes != item.bytes.size())
                        return false;
                    ++members;
                }
                previous = id; id = page.next;
            }
            return previous == root.tail && members == count;
        }) == db::batch_status::ok, "whole members fragmented at page end");
        for (unsigned i = 0; i < count; ++i) {
            require(operation(database, "whole", [&](list_store& list) {
                std::string value;
                return list.pop_one(!left, value) &&
                    value == (left ? expected.back() : expected.front());
            }), "whole member pop");
            if (left) expected.pop_back();
            else expected.pop_front();
        }
        require(contents(database, "whole").empty(), "whole member list not empty");
    }
}

static void test_fragment_page_fill() {
    using codec = list_page_codec;
    for (bool left : {false, true}) {
        auto database = memory();
        // The first value leaves room for only one byte and its fragment header.
        std::vector<std::string> values{std::string(codec::max_fragment_bytes -
            codec::fragment_header_bytes - 1, 'x')};
        for (char byte = 'a'; byte <= 'f'; ++byte)
            values.emplace_back(codec::max_fragment_bytes + 1, byte);
        require(operation(database, "split", [&](list_store& list) {
            return list.push(values, left);
        }), "fragment fill push");
        require(database->largest == codec::max_page_bytes - 1,
            "split fragment must leave space for the string terminator");
        if (left) std::reverse(values.begin(), values.end());
        require(contents(database, "split") == values, "fragment fill order");

        const std::string exact(2 * codec::max_fragment_bytes - 1, 'z');
        const auto previous_writes = database->writes;
        require(operation(database, "exact", [&](list_store& list) {
            return list.push({exact}, left);
        }), "exact fragment fill push");
        require(database->writes - previous_writes == 2 &&
            database->largest == codec::max_page_bytes &&
            contents(database, "exact") == std::vector<std::string>{exact},
            "exact fit must not allocate another page");
    }
}

static void test_whole_member_growth_rollback() {
    for (bool left : {false, true}) {
        bool succeeded = false;
        size_t failures = 0;
        for (long budget = 0; budget < 256; ++budget) {
            auto database = memory();
            std::vector<std::string> original(22, std::string(64, 'o'));
            original.push_back("small");
            require(operation(database, "whole", [&](list_store& list) {
                return list.push(original, false);
            }), "whole growth fault seed");
            std::string original_root;
            require(database->get("whole", original_root), "whole root");
            const std::string added(64, 'n');
            const db::page_callback callback = [&](page_transaction& tx) {
                list_store list(tx, "whole");
                return list.open() && list.push({added}, left) &&
                    list.finish();
            };
            std::vector<std::string> keys{"whole"};
            allocation_budget = budget;
            const auto result = database->transact_pages(std::move(keys),
                callback);
            allocation_budget = -1;
            if (result == db::batch_status::ok) {
                auto expected = original;
                expected.insert(left ? expected.begin() : expected.end(),
                    added);
                require(contents(database, "whole") == expected,
                    "whole growth success lost data");
                succeeded = true; break;
            }
            ++failures;
            std::string root;
            require(database->get("whole", root) && root == original_root &&
                contents(database, "whole") == original,
                "whole growth failure published a partial page/link/root");
        }
        require(succeeded && failures, "whole growth fault sweep incomplete");
    }
}

int main() {
    acl::acl_cpp_init();
    test_packing_and_access();
    test_small_push_packing(3, 511);
    test_small_push_packing(16, 1023);
    test_small_push_packing(128);
    test_whole_member_pages(64, 600);
    for (size_t width : {size_t(4097), size_t(5400), list_page_codec::wide_member_bytes,
            size_t(8192), size_t(list_page_codec::max_fragment_bytes)})
        test_whole_member_pages(width, 32);
    test_fragment_page_fill();
    test_whole_member_growth_rollback();
    test_medium_push_packing();
    test_medium_page_growth_rollback();
    test_old_medium_page_append();
    test_wide_page_append();
    test_model();
    test_large_and_reverse();
    test_search_and_trim();
    test_reset_and_legacy_root();
    test_corruption_and_exhaustion();
    test_pop_cycle_rollback();
    test_allocation_rollback();
    test_endpoint_allocation_rollback();
    test_unfinished_context();
    test_yielding_transactions();
    std::cout << "Packed List page operations and atomic resets passed\n";
}
