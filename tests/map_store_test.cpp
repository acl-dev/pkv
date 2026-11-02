#include "stdafx.h"
#include "db/map_store.h"
#include "db/mdb/mdb.h"
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <random>
#include <thread>
#include <set>
#include <new>

static thread_local long allocation_budget = -1;
void* operator new(std::size_t size) {
    if (allocation_budget == 0) throw std::bad_alloc();
    if (allocation_budget > 0) --allocation_budget;
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

static void require(bool ok, const char* reason) {
    if (!ok) { std::cerr << reason << '\n'; std::exit(1); }
}

class tracking_db : public mdb {
public:
    tracking_db() : mdb(4) {}
    size_t reads = 0, blobs = 0, writes = 0, largest = 0;
protected:
    lookup_status dblookup_page(const std::string& owner, uint64_t id,
        std::string& out) override {
        ++reads;
        const auto result = mdb::dblookup_page(owner, id, out);
        if (result == lookup_status::found && out.compare(0, 3, "PMB") == 0)
            ++blobs;
        return result;
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
    const std::function<bool(map_store&)>& callback,
    map_kind kind = map_kind::hash) {
    return database->transact_pages({key}, [&](page_transaction& tx) {
        map_store map(tx, key, kind);
        const bool ok = map.open() && callback(map) && map.finish();
        if (!ok) std::cerr << "map error " << static_cast<int>(map.error()) <<
            " elements " << map.metadata().elements << '\n';
        return ok;
    }) == db::batch_status::ok;
}

static void seed(shared_db database, const fields& expected) {
    require(operation(database, "hash", [&](map_store& map) {
        map.initialize(expiry);
        for (const auto& item : expected) {
            bool added;
            if (!map.put(item.first, item.second, false, added) || !added)
                return false;
        }
        return true;
    }), "seed");
}

struct audit {
    page_transaction& tx;
    std::set<uint64_t> visited;
    fields actual;
    std::string previous;
    bool has_previous = false;
    unsigned previous_bucket = 0;
    explicit audit(page_transaction& view) : tx(view) {}
    std::string blob(const map_blob& value) {
        if (!value.head) return value.inline_value;
        std::string result;
        uint64_t id = value.head;
        while (id) {
            require(visited.insert(id).second, "shared blob/cycle");
            std::string bytes;
            map_fragment page;
            require(tx.lookup("hash", id, bytes) == db::lookup_status::found &&
                map_page_codec::decode_fragment(bytes, page) ==
                    map_page_status::ok && page.id == id && page.offset ==
                    result.size(), "blob chain");
            result += page.bytes;
            id = page.next;
        }
        require(result.size() == value.length, "blob length");
        return result;
    }
    std::pair<unsigned, uint64_t> node(uint64_t id, unsigned depth = 0) {
        if (!id) return {0, 0};
        require(depth < 128 && visited.insert(id).second, "tree cycle/alias");
        std::string bytes;
        map_node value;
        require(tx.lookup("hash", id, bytes) == db::lookup_status::found &&
            bytes.size() <= 16384 && map_page_codec::decode_node(bytes,
                value) == map_page_status::ok && value.id == id, "node page");
        const auto before = node(value.left, depth + 1);
        const auto key = blob(value.key);
        const auto bucket = scan_bucket(key);
        require(bucket == value.bucket && (!has_previous ||
            previous_bucket < bucket || (previous_bucket == bucket &&
                previous < key)), "BST bucket ordering");
        previous = key; previous_bucket = static_cast<unsigned>(bucket);
        has_previous = true;
        require(actual.emplace(key, blob(value.value)).second, "duplicate key");
        const auto after = node(value.right, depth + 1);
        require(value.elements == before.second + after.second + 1 &&
            value.height == std::max(before.first, after.first) + 1 &&
            std::abs(static_cast<int>(before.first) -
                static_cast<int>(after.first)) <= 1, "AVL invariant");
        return {value.height, value.elements};
    }
};

static void verify(shared_db database, const fields& expected,
    bool pages = false) {
    require(database->transact_pages({"hash"}, [&](page_transaction& tx) {
        auto& entry = *tx.roots().find("hash");
        if (!entry.found) return expected.empty();
        map_root root;
        require(entry.value.size() == 48 && map_page_codec::decode_root(
            entry.value, root) == map_page_status::ok &&
            root.expires_at_ms == expiry, "root metadata");
        audit inspection(tx);
        const auto counts = inspection.node(root.tree);
        require(counts.second == root.elements && inspection.actual == expected,
            "tree differs from model");
        if (pages) {
            for (uint64_t id = 1; id <= root.last_id; ++id) {
                std::string bytes;
                const bool found = tx.lookup("hash", id, bytes) ==
                    db::lookup_status::found;
                require(found == (inspection.visited.count(id) != 0),
                    "orphaned physical page");
            }
        }
        return true;
    }) == db::batch_status::ok, "verify");
}

static void test_codec() {
    map_root root;
    root.tree = 1; root.last_id = 3; root.elements = 1;
    root.expires_at_ms = expiry;
    std::string bytes, output = "sentinel";
    require(map_page_codec::encode_root(root, bytes) == map_page_status::ok &&
        bytes.size() == 48 && bytes.substr(0, 4) == std::string("PMR\2", 4),
        "root wire");
    for (size_t length = 0; length < bytes.size(); ++length) {
        map_root out; out.last_id = 99;
        require(map_page_codec::decode_root(bytes.substr(0, length), out) !=
            map_page_status::ok && out.last_id == 99, "truncated root");
    }
    bytes[3] = 3;
    require(map_page_codec::decode_root(bytes, root) ==
        map_page_status::unsupported, "future root");
    bytes[3] = 1;
    require(map_page_codec::decode_root(bytes, root) ==
        map_page_status::unsupported,
        "old lexical prototype was reinterpreted");
    map_node node;
    node.id = 1; node.key.inline_value = std::string("a\0", 2);
    node.key.length = 2;
    node.value.inline_value.assign(128, 'v'); node.value.length = 128;
    require(map_page_codec::encode_node(node, bytes) == map_page_status::ok,
        "node encode");
    for (size_t length = 0; length < bytes.size(); ++length) {
        map_node out; out.id = 99;
        require(map_page_codec::decode_node(bytes.substr(0, length), out) !=
            map_page_status::ok && out.id == 99, "truncated node");
    }
    map_node out;
    require(map_page_codec::decode_node(bytes, out) == map_page_status::ok &&
        out.key.inline_value == node.key.inline_value &&
        out.value.inline_value == node.value.inline_value, "node binary");
    node.left = node.id;
    require(map_page_codec::encode_node(node, output) ==
        map_page_status::invalid && output == "sentinel", "invalid node");
    map_fragment fragment;
    fragment.id = 2; fragment.bytes.assign(16352, 'x');
    require(map_page_codec::encode_fragment(fragment, bytes) ==
        map_page_status::ok && bytes.size() == 16384, "fragment bound");
    fragment.bytes.push_back('x');
    require(map_page_codec::encode_fragment(fragment, output) ==
        map_page_status::invalid && output == "sentinel", "oversized fragment");
}

static void test_model() {
    auto database = memory();
    fields expected;
    seed(database, expected);
    std::mt19937 random(7191);
    for (unsigned step = 0; step < 1600; ++step) {
        auto key = std::to_string(random() % 100);
        if (step % 7 == 0) key = std::string("\0\xff", 2) + key;
        if (step % 11 == 0) key.clear();
        const auto value = random() % 7 ? std::to_string(random()) :
            std::string(20000, static_cast<char>(random()));
        const bool nx = random() % 3 == 0;
        const auto choice = random() % 4;
        const bool accepted = operation(database, "hash", [&](map_store& map) {
            if (!map.exists()) map.initialize(expiry);
            bool found;
            if (choice == 0) {
                if (!map.erase(key, found)) return false;
                require(found == (expected.erase(key) != 0), "erase count");
            } else if (choice == 1) {
                std::string out = "sentinel";
                if (!map.get(key, out, found)) return false;
                require(found == (expected.count(key) != 0) &&
                    (!found || out == expected.at(key)), "get model");
            } else {
                const bool existed = expected.count(key) != 0;
                if (!map.put(key, value, nx, found)) return false;
                require(found == !existed, "put count");
                if (!nx || !existed) expected[key] = value;
            }
            require(map.metadata().elements == expected.size(), "map count");
            return true;
        });
        if (!accepted) std::cerr << "model step " << step << " operation " <<
            choice << " key " << key << '\n';
        require(accepted, "model operation");
        if (step % 40 == 0) verify(database, expected);
    }
    verify(database, expected, true);
    require(database->largest <= 16384, "physical value exceeded page bound");
}

static void test_locality_and_large() {
    auto database = memory();
    fields expected;
    for (unsigned i = 0; i < 10000; ++i)
        expected[std::to_string(100000 + i)] = "v";
    seed(database, expected);
    database->reads = database->writes = 0;
    require(operation(database, "hash", [](map_store& map) {
        bool added;
        return map.put("105000", "changed", false, added) && !added;
    }), "local update");
    require(database->reads < 70 && database->writes == 1,
        "point update rewrote/scanned the Hash");
    expected["105000"] = "changed";
    const std::string key(40000, 'k'), value(1024 * 1024, 'v');
    require(operation(database, "hash", [&](map_store& map) {
        bool added;
        return map.put(key, value, false, added) && added;
    }), "large field/value");
    expected[key] = value;
    database->blobs = 0;
    require(operation(database, "hash", [&](map_store& map) {
        uint64_t length; bool found;
        return map.length("105000", length, found) && found && length == 7;
    }) && database->blobs == 0, "unrelated large values loaded");
    require(operation(database, "hash", [&](map_store& map) {
        uint64_t length; bool found;
        return map.length(key, length, found) && found &&
            length == value.size();
    }), "large field length");
    verify(database, expected, true);
}

static void test_allocation_failure() {
    fields expected;
    for (unsigned i = 0; i < 20; ++i)
        expected[std::to_string(i)] = "base";
    expected["large"] = std::string(40000, 'x');
    for (bool erase : {false, true}) {
        bool success = false;
        unsigned failures = 0;
        for (long budget = 0; budget < 800; ++budget) {
            auto database = memory();
            seed(database, expected);
            std::string before, after;
            require(database->get("hash", before), "failure root fixture");
            const std::string replacement(50000, 'r');
            allocation_budget = budget;
            db::batch_status result;
            try {
            result = database->transact_pages({"hash"},
                [&](page_transaction& tx) {
                map_store map(tx, "hash");
                bool changed;
                return map.open() && (erase ? map.erase("1", changed) :
                    map.put("large", replacement, false, changed)) &&
                    map.finish();
            });
            } catch (const std::bad_alloc&) {
                result = db::batch_status::error;
            }
            allocation_budget = -1;
            if (result == db::batch_status::ok) { success = true; break; }
            ++failures;
            require(database->get("hash", after) && after == before,
                "allocation failure changed root");
            verify(database, expected, true);
        }
        require(success && failures > 10, "allocation sweep missed success");
    }
}

static void test_delete_all_and_set_kind() {
    for (bool reverse : {false, true}) {
        auto database = memory();
        fields expected;
        for (unsigned i = 0; i < 31; ++i)
            expected[std::to_string(i)] = std::string(200, 'v');
        seed(database, expected);
        std::vector<std::string> keys;
        for (const auto& item : expected) keys.push_back(item.first);
        if (reverse) std::reverse(keys.begin(), keys.end());
        for (const auto& key : keys) {
            require(operation(database, "hash", [&](map_store& map) {
                bool removed;
                return map.erase(key, removed) && removed;
            }), "ordered deletion");
            expected.erase(key);
            verify(database, expected, true);
        }
        require(database->transact_pages({"hash"}, [](page_transaction& tx) {
            std::string bytes;
            for (uint64_t id = 1; id < 100; ++id)
                if (tx.lookup("hash", id, bytes) != db::lookup_status::missing)
                    return false;
            return true;
        }) == db::batch_status::ok, "last field retained pages");
    }
    auto database = memory();
    require(operation(database, "set", [](map_store& map) {
        bool added, found;
        std::string out;
        return map.put("member", "", false, added) && added &&
            map.get("member", out, found) && found && out.empty();
    }, map_kind::set), "Set-kind empty payload");
    require(database->transact_pages({"set"}, [](page_transaction& tx) {
        map_store map(tx, "set", map_kind::hash);
        require(!map.open() && map.error() == map_store::status::wrong_kind,
            "wrong map kind accepted");
        return true;
    }) == db::batch_status::aborted, "ignored wrong kind committed");
}

static void overwrite64(std::string& bytes, size_t offset, uint64_t value) {
    for (unsigned i = 0; i < 8; ++i)
        bytes[offset + i] = static_cast<char>(value >> ((7 - i) * 8));
}

static void test_corruption_and_exhaustion() {
    for (unsigned damage = 0; damage < 7; ++damage) {
        auto database = memory();
        seed(database, {{"base", "v"}, {"large", std::string(40000, 'x')}});
        require(database->transact_pages({"hash"}, [&](page_transaction& tx) {
            auto& entry = *tx.roots().find("hash");
            map_root root;
            require(map_page_codec::decode_root(entry.value, root) ==
                map_page_status::ok, "corruption root");
            if (damage < 2) {
                if (!damage) ++root.elements;
                else root.last_id = UINT64_MAX;
                entry.action = db::update_action::write;
                return map_page_codec::encode_root(root, entry.value) ==
                    map_page_status::ok;
            }
            if (damage == 4) return tx.erase("hash", root.tree);
            std::string bytes;
            map_node node;
            auto id = root.tree;
            require(tx.lookup("hash", id, bytes) == db::lookup_status::found &&
                map_page_codec::decode_node(bytes, node) ==
                    map_page_status::ok, "corruption node");
            if (damage >= 5) {
                if (!node.value.head) {
                    id = node.left ? node.left : node.right;
                    require(id && tx.lookup("hash", id, bytes) ==
                        db::lookup_status::found && map_page_codec::decode_node(
                            bytes, node) == map_page_status::ok, "large node");
                }
                require(tx.lookup("hash", node.value.head, bytes) ==
                    db::lookup_status::found, "corruption blob");
                if (damage == 5) overwrite64(bytes, 24, 1);
                else bytes[3] = 2;
                return tx.put("hash", node.value.head, bytes);
            }
            if (damage == 2) overwrite64(bytes, 32, 99ULL << 32);
            else overwrite64(bytes, 16, node.id);
            return tx.put("hash", id, bytes);
        }) == db::batch_status::ok, "corruption injection");
        std::string before, after;
        require(database->get("hash", before), "corrupt root snapshot");
        require(database->transact_pages({"hash"}, [&](page_transaction& tx) {
            map_store map(tx, "hash");
            bool added;
            require(map.open() && !map.put(damage >= 5 ? "large" : "new",
                "changed", false, added), "corrupt tree mutation accepted");
            const auto expected = damage == 1 ? map_store::status::too_large :
                damage == 6 ? map_store::status::unsupported :
                    map_store::status::invalid;
            require(map.error() == expected, "corrupt error classification");
            return true; // Even ignored errors must poison the native commit.
        }) == db::batch_status::aborted, "corrupt transaction committed");
        require(database->get("hash", after) && after == before,
            "corrupt root silently rewritten");
    }
}

static void test_finish_and_concurrency() {
    auto database = memory();
    seed(database, {{"base", "v"}});
    require(database->transact_pages({"hash"}, [](page_transaction& tx) {
        map_store map(tx, "hash");
        bool added;
        return map.open() && map.put("forgotten", "v", false, added);
    }) == db::batch_status::aborted, "unfinished context committed");
    verify(database, {{"base", "v"}}, true);
    std::vector<std::thread> workers;
    for (unsigned t = 0; t < 4; ++t) workers.emplace_back([&, t] {
        for (unsigned f = 0; f < 4; ++f) go[&, t, f] {
            for (unsigned i = 0; i < 20; ++i) {
                const auto key = std::to_string(t) + ":" + std::to_string(f) +
                    ":" + std::to_string(i);
                require(operation(database, "hash", [&](map_store& map) {
                    bool added, found;
                    std::string out;
                    if (!map.put(key, key, false, added) || !added)
                        return false;
                    acl::fiber::yield();
                    return map.get(key, out, found) && found && out == key &&
                        map.erase(key, found) && found;
                }), "concurrent read-own-pages");
            }
        };
        acl::fiber::schedule();
    });
    for (auto& worker : workers) worker.join();
    verify(database, {{"base", "v"}}, true);
}

int main() {
    acl::acl_cpp_init();
    test_codec();
    test_model();
    test_locality_and_large();
    test_allocation_failure();
    test_finish_and_concurrency();
    test_delete_all_and_set_kind();
    test_corruption_and_exhaustion();
    std::cout << "bounded map pages and AVL operations passed\n";
}
