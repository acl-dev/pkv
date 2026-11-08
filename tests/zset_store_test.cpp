#include "stdafx.h"
#include "db/zset_store.h"
#include "coder/record_codec.h"
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

static void test_single_fragment_codec() {
    const std::string payload = std::string(72, 'm') + std::string("\0x", 2);
    std::string legacy, compact;
    require(map_page_codec::encode_fragment(7, 0, 0, payload.data(),
        payload.size(), legacy) == map_page_status::ok &&
        map_page_codec::encode_fragment(7, 0, 0, payload.data(),
        payload.size(), compact, true) == map_page_status::ok &&
        legacy.size() - compact.size() == 24, "single fragment header");
    for (const auto* bytes : {&legacy, &compact}) {
        map_fragment page;
        require(map_page_codec::decode_fragment(*bytes, page) ==
            map_page_status::ok && page.id == 7 && !page.next &&
            !page.offset && page.bytes == payload, "single fragment round trip");
    }
    compact.resize(8);
    map_fragment_view view;
    require(map_page_codec::decode_fragment_view(compact, view) ==
        map_page_status::invalid, "empty compact fragment accepted");
}

static void test_wide_inline_key_codec() {
    map_node node;
    node.id = 70000;
    node.key.inline_value.assign(127, 'm');
    node.key.length = node.key.inline_value.size();
    std::string bytes;
    map_node decoded;
    require(map_page_codec::encode_packed_node(node, bytes, false) ==
        map_page_status::ok &&
        map_page_codec::decode_node(bytes, decoded) == map_page_status::ok &&
        decoded.id == node.id && !decoded.key.head &&
        decoded.key.inline_value == node.key.inline_value,
        "wide inline score key round trip");

    node.key.inline_value.clear();
    node.key.length = 72;
    node.key.head = 8;
    require(map_page_codec::encode_node(node, bytes) == map_page_status::ok &&
        map_page_codec::decode_node(bytes, decoded) == map_page_status::ok &&
        decoded.key.head == 8 && decoded.key.length == 72,
        "legacy fragmented key remains readable");
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
        std::string payload;
        require(zset_page_codec::encode_value(score, payload), "encode member score");
        const auto unaligned = std::string("x") + payload;
        require(zset_page_codec::decode_value(unaligned.data() + 1, 8, decoded) &&
            decoded == score && std::signbit(decoded) == std::signbit(score),
            "borrowed member score roundtrip or signed zero");
    }
    std::sort(ordered.begin(), ordered.end());
    for (size_t i = 1; i < ordered.size(); ++i)
        require(ordered[i - 1].second <= ordered[i].second,
            "score byte order differs from numeric order");
    std::string unchanged = "unchanged";
    for (size_t length = 0; length <= 9; ++length) if (length != 8) {
        const std::string bytes(9, '\0');
        double output = 13;
        require(!zset_page_codec::decode_value(bytes.data(), length, output) &&
            output == 13, "borrowed member score length or output changed");
    }
    for (char fill : {char(0), char(255)}) {
        const std::string bytes(8, fill);
        double output = 13;
        require(!zset_page_codec::decode_value(bytes.data(), bytes.size(), output) &&
            output == 13, "borrowed NaN accepted or output changed");
    }
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
        && bytes.size() == 21 && bytes[3] == 2, "compact expiring root");
    for (size_t size = 0; size < bytes.size(); ++size) {
        zset_root decoded;
        require(zset_page_codec::decode_root(bytes.substr(0, size), decoded)
            != map_page_status::ok, "truncated root accepted");
    }
    zset_root decoded;
    require(zset_page_codec::decode_root(bytes, decoded) == map_page_status::ok
        && decoded.expires_at_ms == deadline, "compact root expiry");
    auto corrupt = bytes; corrupt[4] = 3;
    require(zset_page_codec::decode_root(corrupt, root) ==
        map_page_status::invalid, "reserved root byte accepted");
    bytes[3] = 127;
    require(zset_page_codec::decode_root(bytes, root) ==
        map_page_status::unsupported, "future root version accepted");
    root.expires_at_ms = -1;
    root.last_id = UINT16_MAX;
    require(zset_page_codec::encode_root(root, bytes) == map_page_status::ok
        && bytes.size() == 13 && bytes[3] == 2 &&
        zset_page_codec::decode_root(bytes, decoded) == map_page_status::ok
        && decoded.expires_at_ms == -1 && decoded.last_id == UINT16_MAX,
        "compact root counter limit");
    ++root.last_id;
    require(zset_page_codec::encode_root(root, bytes) == map_page_status::ok
        && bytes.size() == 64 && bytes[3] == 1 &&
        zset_page_codec::decode_root(bytes, decoded) == map_page_status::ok
        && decoded.last_id == root.last_id, "wide root counter");
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
    for (unsigned command = 0; command < 5; ++command) {
        bool failed = false, success = false;
        for (long budget = 0; budget < 2000 && !success; ++budget) {
            auto database = memory();
            const auto survivor = command == 4 ? std::string(96, 'c') : "c";
            const values initial = {{"a", -1}, {"b", 2}, {survivor, 2}};
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
                    if (command == 3) return store.put("new-member", 18,
                        added, changed);
                    if (command == 4) {
                        std::vector<zset_member> popped;
                        return store.pop_rebuild(2, false, popped) &&
                            popped.size() == 2 && popped[0].member == "a" &&
                            popped[1].member == "b";
                    }
                    return store.clear() && store.put("replacement", -4,
                        added, changed);
                });
            } catch (const std::bad_alloc&) {}
            allocation_budget = -1;
            if (ok) {
                values expected = initial;
                if (command == 0) expected["a"] = 300;
                else if (command == 1) expected.erase("b");
                else if (command == 2) expected = {{"replacement", -4}};
                else if (command == 3) expected["new-member"] = 18;
                else expected = {{survivor, 2}};
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

static std::map<uint64_t, std::string> raw_pages(shared_db database,
    const std::string& key = "zset") {
    std::map<uint64_t, std::string> result;
    require(database->get(key, result[0]), "snapshot root");
    zset_root root;
    require(zset_page_codec::decode_root(result[0], root) ==
        map_page_status::ok, "snapshot metadata");
    require(database->transact_pages({key}, [&](page_transaction& tx) {
        for (uint64_t id = 1; id <= root.last_id; ++id) {
            std::string bytes;
            const auto status = tx.lookup(key, id, bytes);
            if (status == db::lookup_status::error) return false;
            if (status == db::lookup_status::found) result[id] = bytes;
        }
        return true;
    }) == db::batch_status::ok, "snapshot pages");
    return result;
}

static void test_checked_update() {
    auto database = memory();
    const values initial = {{"a", -1}, {"b", 2}, {"c", -0.0}};
    seed(database, "zset", initial);
    const auto before = raw_pages(database);
    require(operation(database, "zset", [&](zset_store& store) {
        bool added, changed;
        return store.put("b", 2, added, changed) && !added && !changed &&
            store.put("c", 0.0, added, changed) && !added && !changed;
    }) && raw_pages(database) == before, "same-score write changed bytes");
    require(operation(database, "zset", [&](zset_store& store) {
        for (double score : {10.0, -10.0, 0.5}) {
            bool added, changed;
            if (!store.put("b", score, added, changed) || added || !changed)
                return false;
        }
        return true;
    }), "inline score replacements");
    auto updated = initial;
    updated["b"] = 0.5;
    inspect(database, updated);
    const auto after = raw_pages(database);
    require(after.size() == before.size(), "score replacement changed page count");
    for (const auto& page : before)
        require(after.count(page.first) == 1, "score replacement changed page ID");
    for (double score : {20.0, -20.0, -1.0}) {
        require(operation(database, "zset", [&](zset_store& store) {
            bool added, changed;
            return store.put("a", score, added, changed) && !added && changed;
        }), "move score across both branches");
        updated["a"] = score;
        inspect(database, updated);
    }
    for (unsigned damage = 0; damage < 4; ++damage) {
        seed(database, "zset", initial);
        require(database->transact_pages({"zset"}, [&](page_transaction& tx) {
            zset_root root;
            if (zset_page_codec::decode_root(tx.roots().find("zset")->value,
                root) != map_page_status::ok) return false;
            std::string target = "b";
            if (damage != 1 && !zset_page_codec::score_key(2, "b", target))
                return false;
            for (uint64_t id = 1; id <= root.last_id; ++id) {
                std::string bytes;
                map_node node;
                if (tx.lookup("zset", id, bytes) != db::lookup_status::found
                    || map_page_codec::decode_node(bytes, node) !=
                        map_page_status::ok || node.key.inline_value !=
                            target) continue;
                if (damage == 2) return tx.erase("zset", id);
                node.value.length = 3; node.value.inline_value = "bad";
                if (damage == 3) {
                    node.value.length = 5000000;
                    node.value.inline_value.clear();
                    node.value.head = node.id == 1 ? 2 : 1;
                }
                return map_page_codec::encode_node(node, bytes) ==
                    map_page_status::ok && tx.put("zset", id, bytes);
            }
            return false;
        }) == db::batch_status::ok, "checked-update damaged fixture");
        const auto damaged = raw_pages(database);
        for (double score : {2.0, 22.0}) {
            std::vector<watch_token> tokens;
            require(database->watch({"zset"}, tokens) ==
                db::batch_status::ok, "checked update WATCH");
            require(database->transact_pages({"zset", "side"},
                [&](page_transaction& tx) {
                    auto& side = *tx.roots().find("side");
                    side.action = db::update_action::write;
                    side.value = "unpublished";
                    if (!tx.put("side", 1, "unpublished")) return false;
                    zset_store store(tx, "zset");
                    bool added = true, changed = true;
                    require(store.open() && !store.put("b", score, added,
                        changed) && !added && !changed && store.error() ==
                            zset_store::status::invalid,
                        "checked update ignored broken index");
                    return true; // An ignored error must poison all roots.
                }) == db::batch_status::aborted &&
                    raw_pages(database) == damaged,
                "checked-update failure changed raw pages");
            require(tokens.front()->valid(unix_time_ms()),
                "failed checked update invalidated WATCH");
            std::string bytes;
            require(!database->get("side", bytes), "peer root committed");
        }
        require(database->transact_pages({"zset"}, [&](page_transaction& tx) {
            zset_store store(tx, "zset");
            bool removed = true;
            require(store.open() && !store.erase("b", removed) && !removed &&
                store.error() == zset_store::status::invalid,
                "erase ignored broken index");
            return true;
        }) == db::batch_status::aborted && raw_pages(database) == damaged,
            "failed erase changed raw pages");
        require(database->transact_pages({"zset"}, [&](page_transaction& tx) {
            zset_store store(tx, "zset");
            zset_member out{"sentinel", 123};
            require(store.open() && !store.pop_edge(true, out) &&
                store.error() == zset_store::status::invalid &&
                out.member == "sentinel" && out.score == 123,
                "pop ignored broken index or published output");
            return true;
        }) == db::batch_status::aborted && raw_pages(database) == damaged,
            "failed pop changed raw pages");
        require(database->transact_pages({"zset"}, [&](page_transaction& tx) {
            zset_store store(tx, "zset");
            std::vector<zset_member> out{{"sentinel", 123}};
            require(store.open() && !store.pop_rebuild(3, true, out) &&
                store.error() == zset_store::status::invalid &&
                out.size() == 1 && out.front().member == "sentinel",
                "whole-set pop ignored broken index or published output");
            return true;
        }) == db::batch_status::aborted && raw_pages(database) == damaged,
            "failed whole-set pop changed raw pages");
        if (damage == 0) {
            require(database->transact_pages({"zset"}, [&](page_transaction& tx) {
                zset_store store(tx, "zset");
                zset_member out;
                require(store.open() && store.pop_edge(false, out) &&
                    out.member == "a" && store.pop_edge(false, out) &&
                    out.member == "c", "earlier pops could not be staged");
                require(!store.pop_edge(false, out) && out.member == "c" &&
                    store.error() == zset_store::status::invalid,
                    "last pop accepted corruption or changed its output");
                return true;
            }) == db::batch_status::aborted && raw_pages(database) == damaged,
                "failed final pop published earlier deletions");
        }
    }
}

static void test_tree_construction_failure() {
    const std::string key = std::string(256, 'o') + std::string("\0key", 4);
    const values initial = {{"a", -1}, {"b", 2}, {"c", 3}};
    bool failed = false, success = false;
    for (long budget = 0; budget < 10 && !success; ++budget) {
        auto database = memory();
        seed(database, key, initial);
        const auto before = raw_pages(database, key);
        bool caught = false;
        auto status = db::batch_status::error;
        // A fresh worker starts with an empty tree pool. Embedded trees
        // borrow the bound owner, so long keys no longer allocate copies.
        std::thread worker([&] {
            status = database->transact_pages({key, "peer"},
                [&](page_transaction& tx) {
                    auto& peer = *tx.roots().find("peer");
                    peer.action = db::update_action::write;
                    peer.value = "unpublished";
                    if (!tx.put("peer", 1, "unpublished")) return false;
                    zset_store store(tx, key);
                    allocation_budget = budget;
                    try {
                        const bool opened = store.open();
                        allocation_budget = -1;
                        return opened && store.finish();
                    } catch (const std::bad_alloc&) {
                        allocation_budget = -1;
                        caught = true;
                        require(store.error() == zset_store::status::storage &&
                            !store.finish(), "partial tree context stayed usable");
                        return true; // Ignoring the error must abort every root.
                    }
                });
            allocation_budget = -1;
        });
        worker.join();
        require(raw_pages(database, key) == before &&
            contents(database, key) == initial,
            "tree construction failure changed original pages");
        if (caught) {
            failed = true;
            std::string bytes;
            require(status == db::batch_status::aborted &&
                !database->get("peer", bytes),
                "tree construction failure committed peer edits");
        } else {
            require(status == db::batch_status::ok,
                "tree construction did not recover");
            success = true;
        }
    }
    require(failed && success, "incomplete tree-construction fault sweep");
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
    const std::string first(80, 'a'), middle(80, 'b'), last(80, 'c');
    seed(database, "fragment", {{middle, 1}, {first, 0}, {last, 2}});
    std::string bytes;
    zset_root before, after;
    require(database->get("fragment", bytes) &&
        zset_page_codec::decode_root(bytes, before) == map_page_status::ok,
        "fragmented score root");
    require(operation(database, "fragment", [&](zset_store& store) {
        bool added, changed;
        return store.put(middle, 3, added, changed) && !added && changed;
    }) && database->get("fragment", bytes) &&
        zset_page_codec::decode_root(bytes, after) == map_page_status::ok &&
        after.last_id == before.last_id && after.expires_at_ms == deadline &&
        contents(database, "fragment") == values({{middle, 3}, {first, 0}, {last, 2}}),
        "score replacement reuses a fragmented key without changing other members");
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
        zset_member value;
        return store.pop_edge(false, value) && value.member == "a" &&
            value.score == -INFINITY && store.pop_edge(false, value) &&
            value.member.empty() && value.score == 0 && std::signbit(value.score);
    }), "edge pop preserves the member score's negative zero");
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
    require(database->transact_pages({"zset"}, [&](page_transaction& tx) {
        zset_store store(tx, "zset");
        zset_rank rank;
        require(store.open() && !store.rank(damaged_member, rank) &&
            !rank.found && store.error() == zset_store::status::invalid,
            "rank accepted a nonempty score-index value");
        return true;
    }) == db::batch_status::aborted, "rank corruption did not abort");
    require(database->transact_pages({"zset"}, [&](page_transaction& tx) {
        zset_store store(tx, "zset");
        const std::string missing = "missing";
        std::vector<const std::string*> members(128, &damaged_member);
        members.front() = &missing;
        std::vector<zset_score> scores(members.size());
        require(store.open() && !store.get_scores(members.data(), members.size(),
                    scores.data()) && store.error() == zset_store::status::invalid,
            "batch score lookup accepted a nonempty score-index value");
        return true;
    }) == db::batch_status::aborted, "batch score corruption did not abort");
    require(database->transact_pages({"zset"}, [&](page_transaction& tx) {
        zset_store store(tx, "zset");
        zset_member member;
        bool found;
        // This fixture's member order matches its score order.
        const auto rank = std::distance(initial.begin(),
            initial.find(damaged_member));
        require(store.open() && !store.select(rank, member, found) && !found &&
            store.error() == zset_store::status::invalid,
            "selection accepted a nonempty score-index value");
        return true;
    }) == db::batch_status::aborted, "selection corruption did not abort");
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

static void test_wide_pop() {
    auto database = memory();
    const std::string key = "wide-pop";
    values expected;
    std::vector<std::string> by_score(4096);
    for (unsigned i = 0; i < by_score.size(); ++i) {
        const auto score = (i * 4051) % by_score.size();
        by_score[score] = "member:" + std::to_string(10000 + i);
        expected.emplace(by_score[score], score);
    }
    seed(database, key, expected);
    bool staged = false;
    require(!operation(database, key, [&](zset_store& store) {
        std::vector<zset_member> popped;
        staged = store.pop_edges(256, true, popped) && popped.size() == 256;
        return false;
    }) && staged && contents(database, key) == expected,
        "aborted wide pop changed indexes");
    size_t low = 0, high = by_score.size();
    for (unsigned count : {256U, 512U}) {
        const bool maximum = count == 512;
        std::vector<zset_member> popped;
        require(operation(database, key, [&](zset_store& store) {
            return store.pop_edges(count, maximum, popped);
        }) && popped.size() == count, "wide pop commit");
        for (const auto& item : popped) {
            const auto score = maximum ? --high : low++;
            require(item.member == by_score[score] && item.score == score,
                "wide pop score or member order");
            expected.erase(item.member);
        }
        require(contents(database, key) == expected, "wide pop survivor indexes");
    }
}

static void test_rank_key_boundaries() {
    for (size_t width : {56U, 57U, 119U, 120U}) {
        auto database = memory();
        const std::string key = "rank-boundary";
        values expected;
        for (unsigned i = 0; i < 64; ++i)
            expected.emplace(std::string(width - 8, 'm') +
                std::to_string(10000000 + i), i % 5);
        if (width == 119) { expected["a"] = 0; expected["z"] = 6; }
        seed(database, key, expected);
        const auto ordered = ordered_zset(expected);
        const auto check_ranks = [&] {
            execution_guard admission;
            db::page_lock_scope scope(*database, key);
            for (size_t i = 0; i < ordered.size(); ++i) {
                scope.cache_repeated_map_read(&ordered[i].member);
                require(scope.transact(*database, key, [&](page_transaction& tx) {
                    zset_store store(tx, key);
                    zset_rank rank;
                    if (!store.open() || !store.rank(ordered[i].member, rank) || !rank.found ||
                        rank.rank != static_cast<long long>(i) ||
                        rank.score != ordered[i].score) return false;
                    return store.finish();
                }) == db::batch_status::ok, "scoped rank boundary order");
            }
            require(scope.transact(*database, key, [&](page_transaction& tx) {
                zset_store store(tx, key);
                zset_rank missing;
                return store.open() && store.rank(std::string(width, 'z'), missing) &&
                    !missing.found && store.finish();
            }) == db::batch_status::ok, "scoped rank missing member");
        };
        check_ranks();
        if (width == 120) continue; // The score keys already use fragments.
        zset_root root;
        map_node node;
        std::string original;
        require(database->transact_pages({key}, [&](page_transaction& tx) {
            return zset_page_codec::decode_root(tx.roots().find(key)->value,
                root) == map_page_status::ok &&
                tx.lookup(key, root.scores, original) == db::lookup_status::found &&
                map_page_codec::decode_node(original, node) == map_page_status::ok;
        }) == db::batch_status::ok && !node.key.head, "rank boundary root");
        const auto root_member = node.key.inline_value.substr(8);
        require(database->transact_pages({key}, [&](page_transaction& tx) {
            auto damaged = node;
            damaged.value.length = 1; damaged.value.inline_value = "x";
            std::string bytes;
            return map_page_codec::encode_node(damaged, bytes) ==
                map_page_status::ok && tx.put(key, root.scores, std::move(bytes));
        }) == db::batch_status::ok, "damage boundary score value");
        {
            execution_guard admission;
            db::page_lock_scope scope(*database, key);
            require(scope.transact(*database, key, [&](page_transaction& tx) {
                zset_store store(tx, key);
                zset_rank rank;
                require(store.open() && !store.rank(root_member, rank) &&
                    !rank.found && store.error() == zset_store::status::invalid,
                    "rank boundary accepted nonempty score value");
                return true;
            }) == db::batch_status::aborted, "rank boundary corruption committed");
        }
        require(database->transact_pages({key}, [&](page_transaction& tx) {
            return tx.put(key, root.scores, original);
        }) == db::batch_status::ok, "restore boundary score value");
        if (width != 119) continue;
        // Older writers fragmented these keys above 64 bytes. A borrowed
        // traversal must restart on that representation without losing rank.
        require(database->transact_pages({key}, [&](page_transaction& tx) {
            const auto id = ++root.last_id;
            std::string fragment, bytes;
            if (map_page_codec::encode_fragment(id, 0, 0,
                node.key.inline_value.data(), node.key.inline_value.size(),
                fragment) != map_page_status::ok) return false;
            node.key.head = id; node.key.inline_value.clear();
            if (map_page_codec::encode_node(node, bytes) != map_page_status::ok ||
                !tx.put(key, id, std::move(fragment)) ||
                !tx.put(key, root.scores, std::move(bytes))) return false;
            auto* entry = tx.roots().find(key);
            if (zset_page_codec::encode_root(root, entry->value) !=
                map_page_status::ok) return false;
            entry->action = db::update_action::write;
            return true;
        }) == db::batch_status::ok, "legacy fragmented rank root");
        check_ranks();
    }
}

static void test_mixed_key_widths() {
    auto name = [](unsigned id) {
        const size_t widths[] = {20, 112, 120, 160};
        std::string member(widths[id % 4], 'm');
        member[8] = '\0'; member[9] = static_cast<char>(0x80);
        const auto suffix = std::to_string(id);
        member.replace(member.size() - suffix.size(), suffix.size(), suffix);
        return member;
    };
    auto database = memory();
    values expected;
    for (unsigned i = 0; i < 128; ++i) expected[name(i)] = i;
    seed(database, "zset", expected);
    std::mt19937 random(2067);
    for (unsigned step = 0; step < 512; ++step) {
        const auto member = name(random() % 256);
        const auto previous = expected.find(member);
        if (step % 5 == 0) {
            const bool present = previous != expected.end();
            require(operation(database, "zset", [&](zset_store& store) {
                bool removed;
                return store.erase(member, removed) && removed == present;
            }), "mixed-width erase");
            expected.erase(member);
        } else {
            const double score = static_cast<int>(random() % 4096) - 2048;
            const bool inserted = previous == expected.end();
            const bool replaced = inserted || previous->second != score;
            require(operation(database, "zset", [&](zset_store& store) {
                bool added, changed;
                return store.put(member, score, added, changed) &&
                    added == inserted && changed == replaced;
            }), "mixed-width score update");
            expected[member] = score;
        }
        if (step % 64 == 63) inspect(database, expected);
    }
    bool failed = false, succeeded = false;
    const auto incoming = name(513);
    values initial;
    for (unsigned i = 0; i < 32; ++i) initial[name(i)] = i;
    for (long budget = 0; budget < 200 && !succeeded; ++budget) {
        auto target = memory();
        seed(target, "zset", initial);
        const auto before = raw_pages(target);
        bool ok = false;
        allocation_budget = budget;
        try {
            ok = operation(target, "zset", [&](zset_store& store) {
                bool added, changed;
                return store.put(incoming, 1024, added, changed) && added && changed;
            });
        } catch (const std::bad_alloc&) {}
        allocation_budget = -1;
        if (ok) {
            auto result = initial;
            result[incoming] = 1024;
            inspect(target, result);
            succeeded = true;
        } else {
            require(raw_pages(target) == before, "mixed-width failure changed pages");
            inspect(target, initial);
            failed = true;
        }
    }
    require(failed && succeeded, "mixed-width allocation sweep incomplete");
}

int main() {
    acl::acl_cpp_init();
    test_score_codec();
    test_single_fragment_codec();
    test_wide_inline_key_codec();
    test_model();
    test_checked_update();
    test_tree_construction_failure();
    test_abandoned_and_corrupt_contexts();
    test_scan_and_special_scores();
    test_yielding_transactions();
    test_failure_rollback();
    test_large_and_locality();
    test_wide_pop();
    test_rank_key_boundaries();
    test_mixed_key_widths();
#if defined(HAS_TBB)
    use_tbb = true;
    test_mixed_key_widths();
    test_rank_key_boundaries();
    test_model();
    test_checked_update();
    test_tree_construction_failure();
    test_abandoned_and_corrupt_contexts();
    test_scan_and_special_scores();
    test_yielding_transactions();
    test_failure_rollback();
#endif
    std::cout << "Native ZSet dual index, score order and rollback passed\n";
}
