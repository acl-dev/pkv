#include "stdafx.h"
#include "db/zset_store.h"
#include "common/redis_double.h"
#include "db/mdb/mdb.h"
#include <cmath>
#include <cfloat>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <random>
#include <thread>

static thread_local long allocation_budget = -1;
void* operator new(std::size_t size) {
    if (allocation_budget == 0) throw std::bad_alloc();
    if (allocation_budget > 0) --allocation_budget;
    if (void* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
#if __cplusplus >= 201402L
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
#endif

using namespace pkv;
using values = std::map<std::string, double>;
static const long long deadline = 4102444800123LL;
static bool use_tbb = false;
static void require(bool ok, const char* reason) {
    if (!ok) { std::cerr << reason << '\n'; std::exit(1); }
}

static shared_db memory() {
    shared_db database;
#if defined(HAS_TBB)
    if (use_tbb) database = db::create_mdb_tbb(4);
#endif
    if (!database) database = db::create_mdb(4);
    require(database->open(""), "memory open");
    return database;
}

static bool operation(shared_db database, const std::string& key,
    const std::function<bool(zset_store&)>& callback) {
    return database->transact_pages({key}, [&](page_transaction& tx) {
        zset_store store(tx, key);
        return store.open() && callback(store) && store.finish();
    }) == db::batch_status::ok;
}

static void seed(shared_db database, const std::string& key,
    const values& expected) {
    require(operation(database, key, [&](zset_store& store) {
        store.initialize(deadline);
        for (const auto& item : expected) {
            bool added, changed;
            if (!store.put(item.first, item.second, added, changed) ||
                !added || !changed) return false;
        }
        return true;
    }), "seed");
}

static values contents(shared_db database, const std::string& key) {
    values result;
    require(operation(database, key, [&](zset_store& store) {
        return store.visit([&](const std::string& member, double score) {
            require(result.emplace(member, score).second, "duplicate member");
            return true;
        });
    }), "contents");
    return result;
}

static void test_score_codec() {
    std::mt19937_64 random(781835);
    std::vector<std::pair<std::string, double>> ordered;
    std::vector<double> scores = {-INFINITY, -DBL_MAX, -1, -DBL_MIN,
        -std::numeric_limits<double>::denorm_min(), -0.0, 0, DBL_MIN, 1,
        DBL_MAX, INFINITY};
    for (unsigned i = 0; i < 10000; ++i) {
        auto bits = random(); double value;
        std::memcpy(&value, &bits, sizeof(value));
        if (!std::isnan(value)) scores.push_back(value);
    }
    for (double score : scores) {
        std::string encoded;
        double decoded = 13;
        require(zset_page_codec::encode_score(score, encoded) &&
            encoded.size() == 8 && zset_page_codec::decode_score(encoded,
                decoded) && decoded == score, "score roundtrip");
        ordered.emplace_back(encoded, decoded);
    }
    std::sort(ordered.begin(), ordered.end());
    for (size_t i = 1; i < ordered.size(); ++i)
        require(ordered[i - 1].second <= ordered[i].second,
            "score byte order differs from numeric order");
    std::string unchanged = "unchanged";
    require(!zset_page_codec::encode_score(NAN, unchanged) &&
        unchanged == "unchanged", "NaN accepted");
    std::string negative_zero(8, '\xff'); negative_zero[0] = 0x7f;
    double output = 13;
    require(!zset_page_codec::decode_score(negative_zero, output) &&
        output == 13, "noncanonical negative zero accepted");
    zset_root root;
    root.elements = 1; root.members = 1; root.scores = 2; root.last_id = 2;
    root.expires_at_ms = deadline;
    std::string bytes;
    require(zset_page_codec::encode_root(root, bytes) == map_page_status::ok
        && bytes.size() == 64, "root encoding");
    for (size_t size = 0; size < bytes.size(); ++size) {
        zset_root decoded;
        require(zset_page_codec::decode_root(bytes.substr(0, size), decoded)
            != map_page_status::ok, "truncated root accepted");
    }
    auto corrupt = bytes; corrupt[4] = 1;
    require(zset_page_codec::decode_root(corrupt, root) ==
        map_page_status::invalid, "reserved root byte accepted");
    bytes[3] = 2;
    require(zset_page_codec::decode_root(bytes, root) ==
        map_page_status::unsupported, "future root version accepted");
}

struct audit {
    page_transaction& tx;
    std::set<uint64_t> visited;
    explicit audit(page_transaction& view) : tx(view) {}
    std::string blob(const map_blob& blob) {
        if (!blob.head) return blob.inline_value;
        std::string result;
        uint64_t id = blob.head;
        while (id) {
            require(visited.insert(id).second, "shared fragment/cycle");
            std::string bytes;
            map_fragment page;
            require(tx.lookup("zset", id, bytes) == db::lookup_status::found
                && bytes.size() <= 16384 &&
                map_page_codec::decode_fragment(bytes, page) ==
                    map_page_status::ok && page.id == id &&
                page.offset == result.size(), "fragment");
            result += page.bytes; id = page.next;
        }
        require(result.size() == blob.length, "fragment length");
        return result;
    }
    std::pair<uint64_t, uint32_t> tree(uint64_t id, bool lexical,
        std::vector<std::pair<std::string, std::string>>& out) {
        if (!id) return {0, 0};
        require(visited.insert(id).second, "shared tree node/cycle");
        std::string bytes;
        map_node node;
        require(tx.lookup("zset", id, bytes) == db::lookup_status::found &&
            bytes.size() <= 16384 && map_page_codec::decode_node(bytes, node)
                == map_page_status::ok && node.id == id, "node");
        auto left = tree(node.left, lexical, out);
        const auto key = blob(node.key), value = blob(node.value);
        require(node.bucket == (lexical ? 0 : scan_bucket(key)), "bucket");
        out.emplace_back(key, value);
        auto right = tree(node.right, lexical, out);
        require(node.elements == left.first + right.first + 1 &&
            node.height == std::max(left.second, right.second) + 1 &&
            std::abs(static_cast<int>(left.second) -
                static_cast<int>(right.second)) <= 1, "AVL metadata");
        return {node.elements, node.height};
    }
};

static void inspect(shared_db database, const values& expected) {
    require(database->transact_pages({"zset"}, [&](page_transaction& tx) {
        const auto& entry = *tx.roots().find("zset");
        require(entry.found == !expected.empty(), "root existence");
        if (expected.empty()) return true;
        zset_root root;
        require(zset_page_codec::decode_root(entry.value, root) ==
            map_page_status::ok && root.elements == expected.size() &&
            root.expires_at_ms == deadline, "root metadata");
        audit check(tx);
        std::vector<std::pair<std::string, std::string>> members, scores;
        check.tree(root.members, false, members);
        check.tree(root.scores, true, scores);
        values actual;
        for (const auto& item : members) {
            double score;
            require(zset_page_codec::decode_value(item.second, score) &&
                actual.emplace(item.first, score).second, "member index");
        }
        require(actual == expected && scores.size() == expected.size(),
            "member/score index counts");
        std::vector<std::string> order;
        for (const auto& item : ordered_zset(expected)) {
            std::string key;
            require(zset_page_codec::score_key(item.score, item.member, key),
                "expected score key");
            order.push_back(key);
        }
        for (size_t i = 0; i < scores.size(); ++i)
            require(scores[i].first == order[i] && scores[i].second.empty(),
                "score index order/payload");
        for (uint64_t id = 1; id <= root.last_id; ++id) {
            std::string bytes;
            const bool found = tx.lookup("zset", id, bytes) ==
                db::lookup_status::found;
            require(found == (check.visited.count(id) != 0), "orphan page");
        }
        return true;
    }) == db::batch_status::ok, "inspect");
}

static void test_model() {
    auto database = memory();
    values expected;
    std::mt19937 random(637192);
    const std::vector<std::string> members = {"", "a", "b", "c",
        std::string("a\0b", 3), std::string(40000, 'm')};
    for (unsigned step = 0; step < 600; ++step) {
        const auto& member = members[random() % members.size()];
        const bool remove = random() % 3 == 0;
        const double score = static_cast<int>(random() % 200) - 100;
        bool changed, added;
        require(operation(database, "zset", [&](zset_store& store) {
            if (remove) {
                if (!store.erase(member, changed)) return false;
                require(changed == (expected.erase(member) != 0), "erase");
            } else {
                const auto old = expected.find(member);
                const bool missing = old == expected.end();
                const bool update = missing || old->second != score;
                if (!store.put(member, score, added, changed)) return false;
                require(added == missing && changed == update, "put result");
                expected[member] = score;
                if (!store.set_expiry(deadline)) return false;
            }
            const auto ordered = ordered_zset(expected);
            for (size_t rank = 0; rank < ordered.size(); ++rank) {
                zset_member selected;
                zset_rank position;
                bool found;
                require(store.select(rank, selected, found) && found &&
                    selected.member == ordered[rank].member &&
                    selected.score == ordered[rank].score &&
                    store.rank(selected.member, position) && position.found
                    && position.rank == static_cast<long long>(rank),
                    "rank/select roundtrip");
            }
            return true;
        }), "model operation");
        inspect(database, expected);
        require(contents(database, "zset") == expected, "model contents");
    }
}

static void test_failure_rollback() {
    for (unsigned command = 0; command < 3; ++command) {
        bool failed = false, success = false;
        for (long budget = 0; budget < 2000 && !success; ++budget) {
            auto database = memory();
            const values initial = {{"a", -1}, {"b", 2}, {"c", 2}};
            seed(database, "zset", initial);
            std::string before, after;
            require(database->get("zset", before), "fault root");
            bool ok = false;
            allocation_budget = budget;
            try {
                ok = operation(database, "zset", [&](zset_store& store) {
                    bool added, changed;
                    if (command == 0) return store.put("a", 300, added,
                        changed);
                    if (command == 1) return store.erase("b", changed);
                    return store.clear() && store.put("replacement", -4,
                        added, changed);
                });
            } catch (const std::bad_alloc&) {}
            allocation_budget = -1;
            if (ok) {
                values expected = initial;
                if (command == 0) expected["a"] = 300;
                else if (command == 1) expected.erase("b");
                else expected = {{"replacement", -4}};
                require(contents(database, "zset") == expected,
                    "successful fault-sweep command result");
                if (command != 2) inspect(database, expected);
                success = true; continue;
            }
            failed = true;
            require(database->get("zset", after) && after == before &&
                contents(database, "zset") == initial, "failure changed data");
            inspect(database, initial);
        }
        require(failed && success, "incomplete fault sweep");
    }
}

class tracking_db : public mdb {
public:
    tracking_db() : mdb(4) {}
    size_t reads = 0, writes = 0, largest = 0;
protected:
    lookup_status dblookup_page(const std::string& key, uint64_t id,
        std::string& bytes) override {
        ++reads;
        return mdb::dblookup_page(key, id, bytes);
    }
    batch_status dbapply_pages(const std::vector<batch_entry>& roots,
        const std::vector<page_change>& pages) override {
        writes += pages.size();
        for (const auto& page : pages)
            largest = std::max(largest, page.value.size());
        return mdb::dbapply_pages(roots, pages);
    }
};

static void test_large_and_locality() {
    auto tracked = std::make_shared<tracking_db>();
    require(tracked->open(""), "tracking open");
    shared_db database = tracked;
    values expected;
    for (unsigned i = 0; i < 10000; ++i)
        expected[std::to_string(i)] = static_cast<double>(i % 50);
    seed(database, "zset", expected);
    tracked->reads = tracked->writes = 0;
    require(operation(database, "zset", [&](zset_store& store) {
        bool added, changed;
        return store.put("100", -20, added, changed) && !added && changed;
    }) && tracked->writes < 100 && tracked->reads < 500,
        "point update traversed whole collection");
    tracked->reads = tracked->writes = 0;
    require(operation(database, "zset", [&](zset_store& store) {
        return store.set_expiry(deadline + 1);
    }) && !tracked->reads && !tracked->writes, "TTL read/wrote member pages");
    const std::string large(1024 * 1024, 'x');
    require(operation(database, "large", [&](zset_store& store) {
        bool added, changed;
        return store.put(large, INFINITY, added, changed) && added && changed;
    }) && tracked->largest <= 16384, "unbounded member storage");
    require(operation(database, "large", [&](zset_store& store) {
        zset_score score;
        zset_member member;
        bool found;
        return store.get(large, score) && score.found &&
            store.select(0, member, found) && found && member.member == large
            && member.score == INFINITY;
    }), "large member roundtrip");
}

static void test_scan_and_special_scores() {
    auto database = memory();
    const values expected = {{"a", -INFINITY}, {"b", INFINITY}, {"", -0.0},
        {std::string("z\0b", 3), 0}, {std::string(40000, 'x'), 3}};
    seed(database, "zset", expected);
    inspect(database, expected);
    values seen;
    scan_options options;
    options.count = 1;
    do {
        scan_page page;
        require(operation(database, "zset", [&](zset_store& store) {
            return store.scan(options, page);
        }), "native ZSCAN");
        for (size_t i = 0; i < page.values.size(); i += 2) {
            double score;
            require(parse_redis_double(page.values[i + 1], score),
                "scan score text");
            seen.emplace(page.values[i], score);
        }
        options.cursor = page.cursor;
    } while (options.cursor);
    require(seen == expected, "native scan missed members");
    require(operation(database, "zset", [&](zset_store& store) {
        std::vector<zset_member> visited;
        if (!store.visit_ordered([&](const std::string& member, double score) {
            visited.push_back({member, score}); return true;
        })) return false;
        const auto wanted = ordered_zset(expected);
        require(visited.size() == wanted.size(), "ordered visit size");
        for (size_t i = 0; i < wanted.size(); ++i)
            require(visited[i].member == wanted[i].member &&
                visited[i].score == wanted[i].score, "ordered visit");
        unsigned count = 0;
        require(store.visit_ordered([&](const std::string&, double) {
            ++count; return false;
        }) && count == 1, "visitor early stop");
        zset_rank rank;
        require(store.rank("missing", rank) && !rank.found,
            "missing member rank");
        bool found;
        zset_member unchanged{"sentinel", 13};
        return store.select(expected.size(), unchanged, found) && !found &&
            unchanged.member == "sentinel" && unchanged.score == 13;
    }), "ordered view");
    require(!operation(database, "zset", [&](zset_store& store) {
        bool added, changed;
        return store.put("invalid", NAN, added, changed);
    }) && contents(database, "zset") == expected, "NaN modified indexes");
    require(operation(database, "zset", [&](zset_store& store) {
        bool added, changed;
        return store.put("interim", 8, added, changed) && store.clear() &&
            store.put("new", -9, added, changed) &&
            store.set_expiry(deadline);
    }), "clear after dirty updates");
    inspect(database, {{"new", -9}});
    require(operation(database, "zset", [&](zset_store& store) {
        return store.clear();
    }), "clear all");
    std::string missing;
    require(!database->get("zset", missing), "clear retained root");
    require(database->transact_pages({"zset"}, [&](page_transaction& tx) {
        for (uint64_t id = 1; id <= 20; ++id)
            if (tx.lookup("zset", id, missing) != db::lookup_status::missing)
                return false;
        return true;
    }) == db::batch_status::ok, "clear orphan pages");
}

static void test_yielding_transactions() {
    auto database = memory();
    seed(database, "left", {{"counter", 0}});
    seed(database, "right", {{"counter", 0}});
    std::vector<std::thread> workers;
    for (unsigned thread = 0; thread < 4; ++thread) workers.emplace_back([&] {
        for (unsigned fiber = 0; fiber < 4; ++fiber) go[&] {
            for (unsigned step = 0; step < 20; ++step) {
                require(database->transact_pages({"left", "right"},
                    [&](page_transaction& tx) {
                    zset_store left(tx, "left"), right(tx, "right");
                    if (!left.open() || !right.open()) return false;
                    zset_score a, b;
                    if (!left.get("counter", a) || !right.get("counter", b))
                        return false;
                    require(a.found && b.found && a.score == b.score,
                        "observed partial two-key update");
                    bool added, changed;
                    if (!left.put("counter", a.score + 1, added, changed))
                        return false;
                    acl::fiber::yield();
                    return right.put("counter", b.score + 1, added, changed)
                        && left.finish() && right.finish();
                }) == db::batch_status::ok, "yielding dual-index transaction");
            }
        };
        acl::fiber::schedule();
    });
    for (auto& worker : workers) worker.join();
    require(contents(database, "left") == values({{"counter", 320}}) &&
        contents(database, "right") == values({{"counter", 320}}),
        "lost concurrent score updates");
}

static void test_abandoned_and_corrupt_contexts() {
    auto database = memory();
    const values initial = {{"a", -1}, {"b", 2}, {"c", 3}};
    seed(database, "zset", initial);
    require(database->transact_pages({"zset"}, [&](page_transaction& tx) {
        zset_store store(tx, "zset");
        bool added, changed;
        return store.open() && store.put("forgotten", 4, added, changed);
    }) == db::batch_status::aborted &&
        contents(database, "zset") == initial, "unfinished context committed");
    std::string damaged_member;
    require(database->transact_pages({"zset"}, [&](page_transaction& tx) {
        zset_root root;
        if (zset_page_codec::decode_root(tx.roots().find("zset")->value,
            root) != map_page_status::ok) return false;
        std::string bytes;
        map_node node;
        if (tx.lookup("zset", root.scores, bytes) != db::lookup_status::found
            || map_page_codec::decode_node(bytes, node) != map_page_status::ok)
            return false;
        damaged_member = node.key.inline_value.substr(8);
        node.value.length = 1; node.value.inline_value = "x";
        return map_page_codec::encode_node(node, bytes) ==
            map_page_status::ok && tx.put("zset", node.id, bytes);
    }) == db::batch_status::ok, "damage score-index value");
    zset_store::status error = zset_store::status::ok;
    std::string before, after;
    require(database->get("zset", before), "damaged root");
    require(database->transact_pages({"zset"}, [&](page_transaction& tx) {
        zset_store store(tx, "zset");
        zset_score score;
        require(store.open() && !store.get(damaged_member, score),
            "score-index inconsistency was ignored");
        error = store.error();
        return true; // Ignoring an error must not permit a commit.
    }) == db::batch_status::aborted && error == zset_store::status::invalid &&
        database->get("zset", after) && after == before,
        "corrupt index was silently repaired");
    seed(database, "zset", initial);
    require(database->transact_pages({"zset"}, [&](page_transaction& tx) {
        auto& root = *tx.roots().find("zset");
        zset_root metadata;
        if (zset_page_codec::decode_root(root.value, metadata) !=
            map_page_status::ok) return false;
        metadata.last_id = UINT64_MAX;
        root.action = db::update_action::write;
        return zset_page_codec::encode_root(metadata, root.value) ==
            map_page_status::ok;
    }) == db::batch_status::ok, "exhausted ID fixture");
    require(database->get("zset", before), "exhausted root");
    require(!operation(database, "zset", [&](zset_store& store) {
        bool added, changed;
        const bool ok = store.put("a", 9, added, changed);
        error = store.error();
        return ok;
    }) && error == zset_store::status::too_large &&
        database->get("zset", after) && before == after &&
        contents(database, "zset") == initial, "ID exhaustion lost indexes");
}

int main() {
    acl::acl_cpp_init();
    test_score_codec();
    test_model();
    test_abandoned_and_corrupt_contexts();
    test_scan_and_special_scores();
    test_yielding_transactions();
    test_failure_rollback();
    test_large_and_locality();
#if defined(HAS_TBB)
    use_tbb = true;
    test_model();
    test_abandoned_and_corrupt_contexts();
    test_scan_and_special_scores();
    test_yielding_transactions();
    test_failure_rollback();
#endif
    std::cout << "Native ZSet dual index, score order and rollback passed\n";
}
