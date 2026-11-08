#include "stdafx.h"
#include <functional>
#include "db/mdb/mdb.h"
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
    resources(); expiration();
    for (bool tbb : {false, true}) {
#ifndef HAS_TBB
        if (tbb) continue;
#endif
        auto db = tbb ? db::create_mdb_tbb(4) : db::create_mdb(4);
        require(db->open(""), "sampling memory open");
        membership(db); collisions(db, tbb); concurrent(db);
    }
    std::cout << "Sampling ranks, workspace, memory, errors and races passed\n";
}
