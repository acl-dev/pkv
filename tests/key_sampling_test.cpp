#include "stdafx.h"
#include <functional>
#include "db/mdb/mdb.h"
#include "db/mdb/mdb_tbb.h"
#include "db/page_workspace.h"
#include "dao/paged_string.h"
#include "dao/json/json_scan.h"
#include "dao/json/json_key.h"
#include "common/set_sampling.h"
#include <climits>
#include <cstdlib>
#include <iostream>
#include <new>
#include <set>
#include <thread>

static thread_local long budget = -1;
void* operator new(std::size_t size) {
    if (!budget) throw std::bad_alloc();
    if (budget > 0) --budget;
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
static void require(bool ok, const char* reason) {
    if (!ok) { std::cerr << reason << '\n'; std::exit(1); }
}
static void put(shared_db db, const std::string& key,
    const std::string& value = "v") {
    dao::paged_string strings;
    require(strings.set(db, key, value), "sampling seed");
}
static std::set<std::string> ranks(shared_db db) {
    uint64_t count = 0;
    require(db->key_count(count) == db::count_status::ok, "sample count");
    std::set<std::string> seen;
    for (uint64_t rank = 0; rank < count; ++rank) {
        bool found = false;
        std::string key;
        require(db->key_at_rank(rank, key, found) == db::scan_status::ok &&
            found && seen.insert(key).second, "rank is not a bijection");
    }
    for (const auto rank : {count, UINT64_MAX}) {
        std::string key = "sentinel";
        bool found = true;
        require(db->key_at_rank(rank, key, found) == db::scan_status::ok &&
            !found && key.empty(), "invalid rank exposed a stale key");
    }
    return seen;
}
static void empty(shared_db db) {
    for (const auto& key : ranks(db)) require(db->del(key), "sample cleanup");
}
static void membership(shared_db db) {
    empty(db);
    std::set<std::string> expected{"", std::string("\0\xff\r\n", 4)};
    for (unsigned i = 0; i < 257; ++i)
        expected.insert("key:" + std::to_string(i));
    for (const auto& key : expected) put(db, key);
    put(db, "key:1", std::string(70000, 'v'));
    require(ranks(db) == expected, "rank names/pages/binary mismatch");
    require(db->del("key:7") && !db->del("absent"), "sampling deletion");
    expected.erase("key:7");
    require(ranks(db) == expected, "rank deletion skew");
    for (unsigned i = 0; i < 600; ++i) {
        bool found = false;
        std::string key;
        require(db->sample_key(key, found) == db::scan_status::ok && found &&
            expected.count(key), "random rank selected a foreign/page key");
    }
    if (db->supports_workspace()) {
        auto workspace = std::make_shared<page_workspace>(db);
        shared_db view = workspace;
        put(view, "key:1", "overwritten"); put(view, "new", "new");
        require(view->del("key:2"), "sampling staged deletion");
        auto staged = expected;
        staged.erase("key:2"); staged.insert("new");
        require(ranks(view) == staged, "workspace duplicate/missing rank");
        require(workspace->commit() == db::batch_status::ok &&
            ranks(db) == staged, "sampling workspace commit");
    }
    empty(db);
    bool found = true;
    std::string key = "sentinel";
    require(db->sample_key(key, found) == db::scan_status::ok && !found &&
        key.empty(), "empty sampling");
}
class observed : public mdb {
public:
    observed() : mdb(4) {}
    bool failed = false, ghost = false, unsupported = false, reject = false;
    lookup_status physical(const std::string& key) {
        std::string bytes; return mdb::dblookup_page(key, 1, bytes);
    }
    size_t payload_reads = 0, attempts = 0;
protected:
    bool dbdel(const std::string& key) override {
        return !reject && mdb::dbdel(key);
    }
    lookup_status dblookup_page(const std::string& key, uint64_t id,
        std::string& out) override {
        ++payload_reads; return mdb::dblookup_page(key, id, out);
    }
    scan_status dbkey_at_rank(uint64_t rank, std::string& out,
        bool& found) override {
        ++attempts;
        if (failed || ghost || unsupported) {
            out = "missing"; found = true;
            return unsupported ? scan_status::unsupported : failed ?
                scan_status::error : scan_status::ok;
        }
        return mdb::dbkey_at_rank(rank, out, found);
    }
};
static void resources() {
    auto source = std::make_shared<observed>();
    shared_db db = source;
    require(db->open(""), "sampling observed open");
    const std::string name(4096, 'k');
    put(db, name, std::string(2 * 1024 * 1024, 'v'));
    source->payload_reads = 0;
    std::string key;
    key.reserve(name.size());
    bool found = false;
    require(db->sample_key(key, found) == db::scan_status::ok,
        "warm sample RNG/admission");
    budget = 0;
    const auto rank = db->key_at_rank(0, key, found);
    const auto random = db->sample_key(key, found);
    budget = -1;
    require(rank == db::scan_status::ok && random == db::scan_status::ok &&
        found && key == name && !source->payload_reads,
        "native root sampling allocated or read a payload");
    std::vector<watch_token> watch;
    require(db->watch({name}, watch) == db::batch_status::ok, "sample watch");
    unsigned failed = 0, succeeded = 0;
    for (long allowance = 0; allowance < 96; ++allowance) {
        dao::json_scan query;
        std::string selected;
        bool present = true;
        budget = allowance;
        const bool ok = query.random_key(db, selected, present);
        budget = -1;
        if (ok) {
            require(present && selected == name, "sampling OOM success");
            ++succeeded;
        } else {
            require(!present && selected.empty(),
                "sampling OOM partial output");
            ++failed;
        }
        require(watch[0]->valid(0) && ranks(db).size() == 1 &&
            !source->payload_reads, "sampling OOM mutated source");
    }
    require(failed && succeeded, "sampling allocation coverage");
    std::cout << "Sampling allocation budgets: failed=" << failed <<
        " succeeded=" << succeeded << '\n';
    for (unsigned mode = 0; mode < 3; ++mode) {
        source->failed = mode == 0; source->unsupported = mode == 1;
        source->ghost = mode == 2; source->attempts = 0;
        dao::json_scan query;
        std::string selected = "sentinel";
        bool present = true;
        require(!query.random_key(db, selected, present) && !present &&
            selected.empty() && source->attempts <= 2,
            "failed/stale index sampling loop or partial output");
    }
    source->failed = source->ghost = source->unsupported = false;
    require(db->set(name, std::string(2 * 1024 * 1024, 'v')),
        "opaque root sampling setup");
    budget = 0;
    const auto opaque = db->sample_key(key, found);
    budget = -1;
    require(opaque == db::scan_status::ok && found && key == name,
        "physical key sampling copied a large opaque root");
}
static void expiration() {
    auto source = std::make_shared<observed>();
    shared_db db = source;
    require(db->open(""), "sampling expiry source");
    put(db, "expired", std::string(70000, 'p'));
    dao::json_key keys;
    require(keys.set_expiry(db, "expired", unix_time_ms() + 10, 0),
        "sampling expiry setup");
    std::vector<watch_token> watch;
    require(db->watch({"expired"}, watch) == db::batch_status::ok &&
        source->physical("expired") == db::lookup_status::found,
        "sampling expiry pages/watch");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    source->payload_reads = 0; source->reject = true;
    std::string key;
    bool found = false;
    dao::json_scan query;
    require(!query.random_key(db, key, found) && !found && key.empty() &&
        source->physical("expired") == db::lookup_status::found &&
        watch[0]->valid(0), "failed expiry lost pages or WATCH");
    source->reject = false;
    require(query.random_key(db, key, found) && !found && key.empty() &&
        source->physical("expired") == db::lookup_status::missing &&
        !watch[0]->valid(0) && !source->payload_reads,
        "random expiry did not reclaim pages without reading payloads");
}
static void collisions(shared_db db, bool tbb) {
    empty(db);
    std::set<std::string> expected;
    for (unsigned i = 0; expected.size() < 32; ++i) {
        const auto key = "collision:" + std::to_string(i);
        const auto hash = tbb ? acl_hash_crc32(key.data(), key.size()) :
            std::hash<std::string>()(key);
        if (hash % 1024) continue;
        put(db, key); expected.insert(key);
    }
    require(ranks(db) == expected, "colliding roots have biased rank coverage");
    std::string key;
    bool found = false;
    for (unsigned i = 0; i < 300; ++i)
        require(db->sample_key(key, found) == db::scan_status::ok && found &&
            expected.count(key), "collision sampler escaped its population");
    key.reserve(64);
    budget = 0;
    const auto sampled = db->sample_key(key, found);
    const auto ranked = db->key_at_rank(17, key, found);
    budget = -1;
    require(sampled == db::scan_status::ok && ranked == db::scan_status::ok &&
        found && expected.count(key), "collision sampler allocated");
    empty(db);
}
static void concurrent(shared_db db) {
    empty(db); put(db, "pair:a");
    std::thread writer([&] {
        for (unsigned i = 0; i < 300; ++i) {
            const auto a = i % 2 ? "pair:b" : "pair:a";
            const auto b = i % 2 ? "pair:a" : "pair:b";
            require(db->transact({a, b}, [&](db::batch_view roots) {
                auto& from = *roots.find(a); auto& to = *roots.find(b);
                if (!from.found) return false;
                to.value = from.value; to.action = db::update_action::write;
                from.action = db::update_action::erase; return true;
            }) == db::batch_status::ok, "sampling atomic transfer");
        }
    });
    for (unsigned i = 0; i < 300; ++i) {
        std::string key;
        bool found = false;
        dao::json_scan query;
        require(query.random_key(db, key, found) && found &&
            (key == "pair:a" || key == "pair:b"), "torn random keyspace");
    }
    writer.join(); empty(db);
}
static void concurrent_index_publication(shared_db db) {
    // Force all four writers to publish buckets in the same shard and words.
    std::vector<std::string> buckets(256);
    std::set<std::string> expected;
    for (unsigned i = 0; expected.size() < buckets.size(); ++i) {
        const auto key = "bucket:" + std::to_string(i);
        const auto hash = acl_hash_crc32(key.data(), key.size());
        if (hash % 4) continue;
        auto& bucket = buckets[(hash / 4) % buckets.size()];
        if (bucket.empty()) { bucket = key; expected.insert(key); }
    }
    std::string stable;
    for (unsigned i = 0; stable.empty(); ++i) {
        const auto key = "stable:" + std::to_string(i);
        if (acl_hash_crc32(key.data(), key.size()) % 4 == 1) stable = key;
    }
    put(db, stable);
    expected.insert(stable);
    std::atomic<unsigned> ready{0};
    std::vector<std::thread> writers;
    for (unsigned lane = 0; lane < 4; ++lane) {
        writers.emplace_back([&, lane] {
            ++ready;
            while (ready.load() != 4) std::this_thread::yield();
            for (size_t i = lane; i < buckets.size(); i += 4) put(db, buckets[i]);
        });
    }
    for (unsigned i = 0; i < 32; ++i) {
        std::vector<std::string> keys;
        unsigned long long next = 0;
        require(db->scan_keys(0, 4096, keys, next) == db::scan_status::ok && !next &&
            std::find(keys.begin(), keys.end(), stable) != keys.end(),
            "concurrent SCAN lost a stable key");
        for (const auto& key : keys)
            require(expected.count(key), "concurrent SCAN returned a foreign key");
    }
    for (auto& writer : writers) writer.join();
    unsigned long long cursor = 0;
    for (const auto& key : buckets) {
        std::vector<std::string> keys;
        unsigned long long next = 0;
        require(db->scan_keys(cursor, 1, keys, next) == db::scan_status::ok &&
            keys.size() == 1 && keys.front() == key && next == cursor + 1,
            "SCAN changed the published bucket cursor");
        cursor = next;
    }
    std::string sibling;
    for (unsigned i = 0; sibling.empty(); ++i) {
        const auto key = "sibling:" + std::to_string(i);
        if (acl_hash_crc32(key.data(), key.size()) % 1024 == 63 * 4) sibling = key;
    }
    put(db, sibling);
    require(db->del(buckets[63]), "SCAN slot first deletion");
    std::vector<std::string> keys;
    require(db->scan_keys(63, 1, keys, cursor) == db::scan_status::ok &&
        keys == std::vector<std::string>{sibling} && cursor == 64,
        "SCAN dropped a slot with a surviving key");
    require(db->del(sibling), "SCAN slot last deletion");
    require(db->scan_keys(63, 1, keys, cursor) == db::scan_status::ok &&
        keys == std::vector<std::string>{buckets[64]} && cursor == 65,
        "SCAN did not advance past a cleared slot");
    put(db, buckets[63]);
    require(ranks(db) == expected, "concurrent rank index lost a bucket");
    dao::json_scan query;
    require(query.visit_all_keys(db, "*", [&](const std::string& key) {
        require(expected.erase(key) == 1, "concurrent traversal emitted a foreign key");
    }) && expected.empty(), "concurrent traversal lost a bucket");
    empty(db);
}
static void tbb_scan_rollback(shared_db db, const std::vector<std::string>& names) {
    require(db->open(""), "SCAN rollback backend open");
    put(db, names.front());
    bool failed = false;
    for (long allowance = 0; allowance < 128; ++allowance) {
        const auto status = db->transact(names, [&](db::batch_view roots) {
            for (size_t i = 1; i < names.size(); ++i) {
                auto& root = *roots.find(names[i]);
                root.value = "new";
                root.action = db::update_action::write;
            }
            budget = allowance;
            return true;
        });
        budget = -1;
        if (status == db::batch_status::ok) {
            require(failed && ranks(db) == std::set<std::string>(names.begin(), names.end()),
                "SCAN index omitted a successful batch");
            empty(db);
            return;
        }
        failed = true;
        require(status == db::batch_status::error && ranks(db) == std::set<std::string>{names.front()},
            "SCAN index exposed a failed batch");
    }
    require(false, "SCAN rollback allocation loop did not finish");
}
static void shared_tbb_table_rollback() {
    // Separate physical shards that now publish into the same owner table.
    std::vector<std::string> names;
    for (unsigned i = 0; names.size() < 3; ++i) {
        const auto key = "shared-table:" + std::to_string(i);
        const auto shard = acl_hash_crc32(key.data(), key.size()) % 256;
        if (shard == names.size() * 8) names.push_back(key);
    }
    tbb_scan_rollback(db::create_mdb_tbb(256), names);
}
#ifdef HAS_TBB
static void idle_tbb_storage() {
    auto db = std::make_shared<mdb_tbb>(256);
    require(db->open(""), "idle storage backend open");
    require(!db->trim_idle_storage(100), "unopened shards retired");
    put(db, "idle:key");
    require(!db->trim_idle_storage(1100), "live storage retired");
    require(db->del("idle:key") && !db->trim_idle_storage(1200),
        "empty storage did not wait");

    put(db, "idle:key");
    require(db->del("idle:key") && !db->trim_idle_storage(2300),
        "reinsertion did not restart the idle period");
    require(!db->trim_idle_storage(3299), "storage retired too early");
    std::vector<watch_token> watched;
    require(db->watch({"idle:key"}, watched) == db::batch_status::ok &&
        watched.size() == 1 && watched[0]->valid(0), "WATCH before trim");
    budget = 0;
    const bool trimmed = db->trim_idle_storage(3300);
    budget = -1;
    require(!trimmed && db->trim_idle_storage(3301),
        "idle storage failed to retry after allocation failure");
    require(!db->trim_idle_storage(4400) && ranks(db).empty() &&
        watched[0]->valid(0), "idle trim changed empty database semantics");

    put(db, "idle:key");
    require(ranks(db) == std::set<std::string>{"idle:key"} &&
        !watched[0]->valid(0), "reopened shard changed SCAN or WATCH");
}
#endif
static void tbb_scan_slot_promotion() {
    auto db = db::create_mdb_tbb(1);
    require(db->open(""), "SCAN promotion backend open");
    std::vector<std::string> names;
    for (unsigned i = 0; names.size() < 3; ++i) {
        const auto key = "collision:" + std::to_string(i);
        if (acl_hash_crc32(key.data(), key.size()) % 256 == 63)
            names.push_back(key);
    }
    const auto check = [&](const std::vector<std::string>& expected) {
        std::vector<std::string> keys;
        unsigned long long next = 0;
        require(db->scan_keys(0, 1, keys, next) == db::scan_status::ok &&
            keys == expected && next == (expected.empty() ? 0 : 64),
            "SCAN changed bucket order or cursor across promotion");
        require(ranks(db) == std::set<std::string>(expected.begin(), expected.end()),
            "SCAN promotion changed ranks");
    };
    const std::string value = "value";
    put(db, names[0]);
    budget = 0;
    const bool second_stored = db->set(names[1], value);
    budget = -1;
    require(!second_stored, "SCAN promotion ignored allocation failure");
    check({names[0]});
    put(db, names[1]);
    long allowance = 0;
    for (; allowance < 32; ++allowance) {
        budget = allowance;
        const auto stored = db->set(names[2], value);
        budget = -1;
        if (stored) break;
        check({names[0], names[1]});
    }
    require(allowance > 0 && allowance < 32, "SCAN promotion allocation loop");
    check(names);
    require(db->del(names[0]), "SCAN promotion first deletion");
    check({names[2], names[1]});
    require(db->del(names[1]), "SCAN promotion second deletion");
    check({names[2]});
    put(db, names[0]);
    put(db, names[1]);
    check({names[2], names[0], names[1]});
    empty(db);
    check({});
}
static void empty_tbb_shards() {
    auto db = db::create_mdb_tbb(256);
    require(db->open(""), "lazy shard backend open");
    const std::string key = "missing";
    std::string value;
    require(db->lookup(key, value) == db::lookup_status::missing, "lazy shard admission");
    std::vector<std::string> page;
    uint64_t count = 99;
    unsigned long long next = 99;
    bool found = true;
    budget = 0;
    const auto lookup = db->lookup(key, value);
    const auto point = db->read_inline_string(key, value);
    const auto counted = db->key_count(count);
    const auto scanned = db->scan_keys(0, 64, page, next);
    const auto sampled = db->sample_key(value, found);
    budget = -1;
    require(lookup == db::lookup_status::missing && point == db::inline_status::missing &&
        counted == db::count_status::ok && !count && scanned == db::scan_status::ok &&
        page.empty() && !next && sampled == db::scan_status::ok && !found,
        "read materialized an empty oneTBB shard");
    put(db, key);
    require(ranks(db) == std::set<std::string>{key}, "write failed to publish a lazy shard");
}
static void uneven_sampling(bool tbb, size_t shards, std::vector<unsigned> remaining) {
    auto db = tbb ? db::create_mdb_tbb(shards) : db::create_mdb(shards);
    require(db->open(""), "uneven sample open");
    std::map<std::string, unsigned> frequencies;
    for (unsigned i = 0; frequencies.size() < 32; ++i) {
        const auto key = "uneven:" + std::to_string(i);
        const auto hash = tbb ? acl_hash_crc32(key.data(), key.size()) :
            std::hash<std::string>()(key);
        const auto shard = hash % shards;
        if (shard >= remaining.size() || !remaining[shard]) continue;
        --remaining[shard];
        put(db, key);
        frequencies.emplace(key, 0);
    }
    if (tbb) {
        // Raise the bound, then remove the temporary keys without emptying
        // the database. Sampling must cover both the stale and refreshed bound.
        std::vector<std::string> temporary;
        for (unsigned i = 0; temporary.size() < 64; ++i) {
            const auto key = "temporary:" + std::to_string(i);
            if (acl_hash_crc32(key.data(), key.size()) % shards) continue;
            put(db, key);
            temporary.push_back(key);
        }
        for (const auto& key : temporary)
            require(db->del(key), "sample bound churn deletion");
    }
    set_sampling_engine().seed(571991);
    std::string key;
    bool found = false;
    for (unsigned i = 0; i < 32768; ++i) {
        require(db->sample_key(key, found) == db::scan_status::ok && found,
            "uneven sample missing");
        auto hit = frequencies.find(key);
        require(hit != frequencies.end(), "uneven sample escaped the population");
        ++hit->second;
    }
    for (const auto& entry : frequencies)
        require(entry.second >= 800 && entry.second <= 1248,
            "uneven sample biased a shard or slot");
    empty(db);
    require(db->sample_key(key, found) == db::scan_status::ok && !found && key.empty(),
        "stale shard bound returned a deleted key");
    put(db, "reinserted");
    require(db->sample_key(key, found) == db::scan_status::ok && found && key == "reinserted",
        "sample missed an insertion after clearing the bound");
}
static void occupied_shard_transitions() {
    auto db = db::create_mdb(320);
    require(db->open(""), "occupied shard test open");
    constexpr std::array<size_t, 6> positions{{0, 63, 64, 255, 256, 319}};
    std::array<std::string, positions.size()> keys;
    std::set<std::string> expected;
    for (unsigned i = 0; expected.size() < positions.size(); ++i) {
        const auto key = "occupied:" + std::to_string(i);
        const auto shard = std::hash<std::string>()(key) % 320;
        const auto position = std::find(positions.begin(), positions.end(), shard);
        if (position == positions.end()) continue;
        auto& slot = keys[static_cast<size_t>(position - positions.begin())];
        if (slot.empty()) { slot = key; expected.insert(key); }
    }
    std::atomic<unsigned> ready{0};
    std::vector<std::thread> writers;
    for (const auto& key : keys) writers.emplace_back([&, key] {
        ++ready;
        while (ready.load() != keys.size()) std::this_thread::yield();
        put(db, key);
    });
    for (auto& writer : writers) writer.join();
    require(ranks(db) == expected, "occupied bitmap lost a published shard");
    for (size_t i = 0; i < keys.size(); ++i) {
        std::string key;
        bool found = false;
        require(db->key_at_rank(i, key, found) == db::scan_status::ok &&
            found && key == keys[i], "occupied bitmap changed rank order");
    }
    for (size_t i = 0; i < keys.size(); i += 2) {
        require(db->del(keys[i]), "occupied shard erase");
        expected.erase(keys[i]);
    }
    require(ranks(db) == expected, "occupied bitmap dropped a surviving shard");
    put(db, keys[2]);
    expected.insert(keys[2]);
    require(ranks(db) == expected, "occupied bitmap lost a reinserted shard");
}
int main(int argc, char** argv) {
    acl::acl_cpp_init();
    if (argc >= 3) {
        const std::string backend = argv[1];
        auto db = backend == "rdb" ? db::create_rdb() :
            backend == "wdb" ? db::create_wdb() : db::create_mdb_tbb(4);
        require(db && db->open(argv[2]), "sampling backend open");
        if (argc == 3) {
            membership(db);
            put(db, "persisted", std::string(70000, 'p'));
        } else {
            require(ranks(db) == std::set<std::string>{"persisted"},
                "sampling reopen rank");
            std::string key; bool found = false;
            dao::json_scan query;
            require(query.random_key(db, key, found) && found &&
                key == "persisted", "sampling reopened selection");
            dao::paged_string strings;
            std::string value;
            require(strings.get(db, key, value) && value ==
                std::string(70000, 'p'), "sampling damaged persisted pages");
        }
        std::cout << "Backend sampling and reopen checks passed\n";
        return 0;
    }
    resources(); expiration(); occupied_shard_transitions();
    std::vector<unsigned> sparse(17, 1);
    sparse.front() = 16;
    for (bool tbb : {false, true}) {
#ifndef HAS_TBB
        if (tbb) continue;
#endif
        uneven_sampling(tbb, 4, {16, 8, 4, 4});
        uneven_sampling(tbb, 256, sparse);
        auto db = tbb ? db::create_mdb_tbb(4) : db::create_mdb(4);
        require(db->open(""), "sampling memory open");
        if (tbb) {
            empty_tbb_shards();
            concurrent_index_publication(db);
            tbb_scan_rollback(db::create_mdb_tbb(4), {"stable", "new:one", "new:two"});
            shared_tbb_table_rollback();
#ifdef HAS_TBB
            idle_tbb_storage();
#endif
            tbb_scan_slot_promotion();
        }
        membership(db); collisions(db, tbb); concurrent(db);
    }
    std::cout << "Sampling ranks, workspace, memory, errors and races passed\n";
}
