#include "stdafx.h"
#include "db/map_store.h"
#include "db/mdb/mdb.h"
#include "db/db_watcher.h"
#include "rpc/page_batch.h"
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <set>
#include <new>

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
using fields = std::map<std::string, std::string>;
static const long long expiry = 4102444800123LL;

static void require(bool ok, const char* why) {
    if (!ok) { std::cerr << why << '\n'; std::exit(1); }
}

class tracking_db : public mdb {
public:
    tracking_db() : mdb(4) {}
    size_t reads = 0, blobs = 0;
protected:
    lookup_status dblookup_page(const std::string& owner, uint64_t id,
        std::string& bytes) override {
        ++reads;
        const auto result = mdb::dblookup_page(owner, id, bytes);
        if (result == lookup_status::found && bytes.compare(0, 3, "PMB") == 0)
            ++blobs;
        return result;
    }
};

static std::shared_ptr<tracking_db> memory() {
    auto database = std::make_shared<tracking_db>();
    require(database->open(""), "open");
    return database;
}

static bool operation(shared_db database, const std::string& key,
    const std::function<bool(map_store&)>& callback,
    map_kind kind = map_kind::hash) {
    return database->transact_pages({key}, [&](page_transaction& tx) {
        map_store map(tx, key, kind);
        return map.open() && callback(map) && map.finish();
    }) == db::batch_status::ok;
}

static void seed(shared_db database, const std::string& key,
    const fields& expected, map_kind kind = map_kind::hash) {
    require(operation(database, key, [&](map_store& map) {
        map.initialize(expiry);
        for (const auto& item : expected) {
            bool added;
            if (!map.put(item.first, item.second, false, added) || !added)
                return false;
        }
        return true;
    }, kind), "seed");
}

static fields contents(shared_db database, const std::string& key,
    map_kind kind = map_kind::hash) {
    fields result;
    require(operation(database, key, [&](map_store& map) {
        require(!map.exists() || map.metadata().expires_at_ms == expiry,
            "changed expiry");
        return map.visit([&](const std::string& name,
            const std::string& value) {
            require(result.emplace(name, value).second, "duplicate field");
            return true;
        });
    }, kind), "contents");
    return result;
}

static db::batch_status clone(shared_db from, const std::string& source,
    shared_db to, const std::string& target, map_kind kind = map_kind::hash) {
    return from->copy_pages_to(source, *to, target,
        [&](page_transaction& source_tx, page_transaction& target_tx) {
        map_store map(source_tx, source, kind), copy(target_tx, target, kind);
        bool copied;
        return map.open() && map.clone_to(copy, copied) && copy.finish();
    });
}

static void test_visit_and_scan() {
    auto database = memory();
    fields expected;
    for (unsigned i = 0; i < 1200; ++i)
        expected[std::to_string(i)] = std::string(2000, 'v');
    expected[""] = "";
    expected[std::string("a\0b", 3)] = "binary";
    seed(database, "hash", expected);
    require(contents(database, "hash") == expected, "visitor model");
    database->reads = database->blobs = 0;
    unsigned seen = 0;
    require(operation(database, "hash", [&](map_store& map) {
        return map.visit([&](const std::string&, const std::string& value) {
            require(value.empty(), "key-only visit loaded values");
            return ++seen < 1;
        }, false);
    }) && seen == 1 && database->reads < 100 && !database->blobs,
        "early visitor scanned full map or loaded value pages");
    fields actual;
    scan_options options;
    options.count = 1;
    unsigned pages = 0;
    do {
        scan_page page;
        require(operation(database, "hash", [&](map_store& map) {
            return map.scan(options, page);
        }), "scan");
        require(page.values.size() % 2 == 0, "scan pairs");
        for (size_t i = 0; i < page.values.size(); i += 2)
            actual[page.values[i]] = page.values[i + 1];
        options.cursor = page.cursor;
        require(++pages <= 256, "nonterminating cursor");
    } while (options.cursor);
    require(actual == expected && pages > 1, "bucket scan coverage");
    options = scan_options(); options.count = SIZE_MAX;
    options.has_match = true; options.pattern = "absent:*";
    database->blobs = 0;
    require(operation(database, "hash", [&](map_store& map) {
        scan_page page;
        return map.scan(options, page) && page.values.empty() && !page.cursor;
    }) && !database->blobs, "MATCH loaded rejected fields' values");
    options.has_match = false; options.no_values = true;
    database->blobs = 0;
    require(operation(database, "hash", [&](map_store& map) {
        scan_page page;
        return map.scan(options, page) && page.values.size() == expected.size();
    }) && !database->blobs, "NOVALUES read external values");
}

static void test_scan_churn() {
    auto database = memory();
    fields stable;
    for (unsigned i = 0; i < 300; ++i)
        stable["stable:" + std::to_string(i)] = "v";
    seed(database, "hash", stable);
    std::set<std::string> seen;
    scan_options options;
    options.count = 1; options.no_values = true;
    unsigned pages = 0;
    do {
        scan_page page;
        require(operation(database, "hash", [&](map_store& map) {
            return map.scan(options, page);
        }), "churning scan");
        seen.insert(page.values.begin(), page.values.end());
        require(operation(database, "hash", [&](map_store& map) {
            for (unsigned i = 0; i < 20; ++i) {
                bool changed;
                if (pages && !map.erase("temp:" + std::to_string(pages - 1) +
                    ":" + std::to_string(i), changed)) return false;
                if (!map.put("temp:" + std::to_string(pages) + ":" +
                    std::to_string(i), "v", false, changed)) return false;
            }
            return true;
        }), "scan churn mutation");
        options.cursor = page.cursor;
        require(++pages <= 256, "churning cursor did not terminate");
    } while (options.cursor);
    for (const auto& item : stable)
        require(seen.count(item.first) != 0, "churn skipped stable field");
    options.cursor = UINT64_MAX;
    require(operation(database, "hash", [&](map_store& map) {
        scan_page page;
        return map.scan(options, page) && !page.cursor && page.values.empty();
    }), "oversized cursor");
}

static void test_small_scan_locality() {
    auto database = memory();
    fields expected;
    for (unsigned i = 0; i < 10000; ++i)
        expected[std::to_string(i)] = "v";
    seed(database, "hash", expected);
    database->reads = 0;
    scan_page page;
    scan_options options;
    options.cursor = 128; options.count = 1; options.no_values = true;
    require(operation(database, "hash", [&](map_store& map) {
        return map.scan(options, page);
    }) && database->reads < 500 && page.cursor > 128 &&
        page.values.size() < 100, "scan walked unrelated subtrees");
}

static fields large_fields() {
    std::string prefix(1024 * 1024, 'k');
    const auto first = prefix + std::string("\0\0", 2);
    const auto bucket = scan_bucket(first);
    std::string second;
    for (unsigned byte = 0; byte < 256; ++byte) {
        const auto candidate = prefix + std::string("\1", 1) +
            std::string(1, static_cast<char>(byte));
        if (scan_bucket(candidate) == bucket) { second = candidate; break; }
    }
    require(!second.empty(), "same-bucket key fixture");
    return {{first, std::string(1024 * 1024, 'v')}, {second, "small"}};
}

static void test_clone_bounded_and_independent() {
    auto source = memory(), target = memory();
    const auto expected = large_fields();
    seed(source, "hash", expected);
    seed(target, "copy", {{"old", "value"}});
    require(target->transact_pages({"copy"}, [](page_transaction& tx) {
        return tx.put("copy", 999999, "orphaned old destination page");
    }) == db::batch_status::ok, "old destination fixture");
    largest_allocation = 0; measure = true;
    const auto result = clone(source, "hash", target, "copy");
    measure = false;
    require(result == db::batch_status::ok && largest_allocation < 32768,
        "clone materialized a large key/value or complete map");
    require(contents(target, "copy") == expected &&
        contents(source, "hash") == expected, "clone contents");
    require(target->transact_pages({"copy"}, [](page_transaction& tx) {
        std::string bytes;
        return tx.lookup("copy", 999999, bytes) == db::lookup_status::missing;
    }) == db::batch_status::ok, "clone retained previous family");
    require(operation(target, "copy", [&](map_store& map) {
        bool added;
        return map.put(expected.begin()->first, "independent", false, added) &&
            !added;
    }) && contents(source, "hash") == expected, "clone shared mutable pages");
    require(clone(source, "hash", source, "same-db") == db::batch_status::ok &&
        contents(source, "same-db") == expected, "same DB clone");
    require(clone(source, "missing", target, "copy") == db::batch_status::ok,
        "empty source clone");
    seed(source, "set", {{"", ""}, {"member", ""}}, map_kind::set);
    require(clone(source, "set", target, "set", map_kind::set) ==
        db::batch_status::ok && contents(target, "set", map_kind::set) ==
            contents(source, "set", map_kind::set), "Set-kind clone");
}

static void test_allocation_rollback() {
    fields expected;
    for (unsigned i = 0; i < 20; ++i)
        expected[std::to_string(i)] = "v";
    expected["large"] = std::string(50000, 'x');
    auto source = memory();
    seed(source, "hash", expected);
    std::string source_before, source_after;
    require(source->get("hash", source_before), "source snapshot");
    unsigned failures = 0;
    bool success = false;
    for (long budget = 0; budget < 1800; ++budget) {
        auto target = memory();
        seed(target, "copy", {{"old", std::string(40000, 'o')}});
        std::string before, after;
        require(target->get("copy", before), "target snapshot");
        allocation_budget = budget;
        db::batch_status result;
        try { result = clone(source, "hash", target, "copy"); }
        catch (const std::bad_alloc&) { result = db::batch_status::error; }
        allocation_budget = -1;
        if (result == db::batch_status::ok) {
            require(contents(target, "copy") == expected, "successful clone");
            success = true; break;
        }
        ++failures;
        require(target->get("copy", after) && after == before &&
            source->get("hash", source_after) &&
            source_after == source_before,
            "failed clone changed roots");
        require(contents(target, "copy") == fields{{"old",
            std::string(40000, 'o')}},
            "failed clone changed destination pages");
    }
    require(success && failures > 20 && contents(source, "hash") == expected,
        "clone allocation sweep");
}

static void test_damaged_source_and_outputs() {
    for (unsigned damage = 0; damage < 3; ++damage) {
        auto database = memory();
        seed(database, "hash", {{"a", std::string(300, 'v')},
            {"b", std::string(300, 'v')}});
        seed(database, "copy", {{"old", "keep"}});
        require(database->transact_pages({"hash"}, [&](page_transaction& tx) {
            map_root root;
            require(map_page_codec::decode_root(tx.roots().find("hash")->value,
                root) == map_page_status::ok, "damage root");
            std::string bytes;
            map_node first, second;
            require(tx.lookup("hash", root.tree, bytes) ==
                db::lookup_status::found && map_page_codec::decode_node(bytes,
                    first) == map_page_status::ok, "damage first node");
            const auto other = first.left ? first.left : first.right;
            require(tx.lookup("hash", other, bytes) == db::lookup_status::found
                && map_page_codec::decode_node(bytes, second) ==
                    map_page_status::ok, "damage second node");
            if (damage == 0) {
                second.value.head = first.value.head; // Cross-field alias.
                require(map_page_codec::encode_node(second, bytes) ==
                    map_page_status::ok, "alias node encode");
                return tx.put("hash", second.id, bytes);
            }
            if (damage == 1) {
                ++first.bucket; // Inline-key bucket no longer matches its key.
                require(map_page_codec::encode_node(first, bytes) ==
                    map_page_status::ok, "bucket node encode");
                return tx.put("hash", first.id, bytes);
            }
            require(tx.lookup("hash", first.value.head, bytes) ==
                db::lookup_status::found, "future blob fixture");
            bytes[3] = 2;
            return tx.put("hash", first.value.head, bytes);
        }) == db::batch_status::ok, "damage injection");
        std::string before, after;
        require(database->get("copy", before), "damage target snapshot");
        require(database->transact_pages({"copy", "hash"},
            [&](page_transaction& tx) {
            map_store source(tx, "hash"), target(tx, "copy");
            bool copied;
            require(source.open() && !source.clone_to(target, copied) &&
                !copied, "damaged graph cloned");
            return true; // Ignoring the failed clone must still abort.
        }) == db::batch_status::aborted && database->get("copy", after) &&
            after == before && contents(database, "copy") ==
                fields{{"old", "keep"}}, "failed clone changed destination");
        if (damage == 1 || damage == 2) {
            scan_page out; out.cursor = 99; out.values = {"sentinel"};
            require(database->transact_pages({"hash"},
                [&](page_transaction& tx) {
                map_store map(tx, "hash");
                scan_options options; options.count = SIZE_MAX;
                require(map.open() && !map.scan(options, out),
                    "damaged scan succeeded");
                return true;
            }) == db::batch_status::aborted && out.cursor == 99 &&
                out.values == std::vector<std::string>{"sentinel"},
                "failed scan leaked partial output");
        }
    }
}

static void test_unfinished_clone_and_invalid_scan() {
    auto database = memory();
    seed(database, "hash", {{"a", "v"}, {"b", "v"}});
    seed(database, "copy", {{"old", "keep"}});
    require(database->transact_pages({"hash", "copy"},
        [](page_transaction& tx) {
        map_store from(tx, "hash"), to(tx, "copy");
        bool copied;
        return from.open() && from.clone_to(to, copied) && copied;
    }) == db::batch_status::aborted && contents(database, "copy") ==
        fields{{"old", "keep"}}, "unfinished clone committed");
    require(database->transact_pages({"hash"}, [](page_transaction& tx) {
        map_store map(tx, "hash");
        scan_options options; options.count = 0;
        scan_page page;
        require(map.open() && !map.scan(options, page), "zero COUNT accepted");
        return true;
    }) == db::batch_status::aborted, "ignored scan failure committed");
    require(database->transact_pages({"hash"}, [](page_transaction& tx) {
        map_store map(tx, "hash");
        bool copied;
        require(map.open() && !map.clone_to(map, copied) && !copied &&
            map.error() == map_store::status::invalid, "self-clone accepted");
        return true;
    }) == db::batch_status::aborted, "ignored self-clone committed");
}

class recorder : public db_watcher {
public:
    std::shared_ptr<const page_batch> batch;
    bool supports_pages() const override { return true; }
    bool on_pages_database(unsigned database,
        const std::vector<db::batch_entry>& roots,
        const std::vector<page_change>& pages) override {
        batch = page_batch::capture(database, roots, pages);
        return batch != nullptr;
    }
    bool on_get(const std::string&, const std::string&, bool) override {
        return true;
    }
    bool on_set(const std::string&, const std::string&, bool) override {
        return true;
    }
    bool on_del(const std::string&, bool) override { return true; }
};

static void test_publication() {
    auto source = memory(), target = memory(), replica = memory();
    const fields expected = {{"a", std::string(70000, 'v')}, {"b", "small"}};
    seed(source, "hash", expected);
    seed(replica, "copy", {{"old", "v"}});
    recorder events;
    db_watchers watchers(7);
    watchers.add_watcher(&events);
    require(target->open("", &watchers), "attach observer");
    require(clone(source, "hash", target, "copy") == db::batch_status::ok &&
        events.batch && events.batch->database() == 7 &&
        events.batch->roots().size() == 1 &&
        events.batch->roots().front().clear_pages, "clone publication");
    for (const auto& page : events.batch->pages())
        require(page.value.size() <= 16384, "unbounded clone event");
    require(events.batch->apply(*replica) == db::batch_status::ok &&
        contents(replica, "copy") == expected, "replicated native clone");
}

int main() {
    acl::acl_cpp_init();
    test_visit_and_scan();
    test_scan_churn();
    test_small_scan_locality();
    test_clone_bounded_and_independent();
    test_allocation_rollback();
    test_publication();
    test_damaged_source_and_outputs();
    test_unfinished_clone_and_invalid_scan();
    std::cout << "bounded map iteration and independent cloning passed\n";
}
