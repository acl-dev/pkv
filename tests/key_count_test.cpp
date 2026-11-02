#include "stdafx.h"
#include "db/key_count.h"
#include "db/page_workspace.h"
#include "db/mdb/mdb.h"
#include "coder/string_page.h"
#include "dao/paged_string.h"
#include "dao/json/json_key.h"
#include <climits>
#include <cstdlib>
#include <iostream>
#include <new>
#include <thread>
#include <unordered_set>

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
void operator delete(void* p, std::size_t) noexcept { ::operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept { ::operator delete(p); }
#endif
using namespace pkv;
static void require(bool ok, const char* text) {
    if (!ok) { std::cerr << text << '\n'; std::exit(1); }
}
static uint64_t count(shared_db database) {
    uint64_t value = UINT64_MAX;
    require(database->key_count(value) == db::count_status::ok,
        "root count unsupported/error"); return value;
}
static uint64_t enumerated(shared_db database) {
    std::unordered_set<std::string> keys;
    unsigned long long cursor = 0;
    do {
        std::vector<std::string> page;
        unsigned long long next;
        require(database->scan_keys(cursor, 1, page, next) ==
            db::scan_status::ok, "root count enumeration");
        keys.insert(page.begin(), page.end()); cursor = next;
    } while (cursor);
    return keys.size();
}
static shared_db memory(bool tbb) {
    auto db = tbb ? db::create_mdb_tbb(4) : db::create_mdb(4);
    require(db->open(""), "count open memory"); return db;
}
static void basic(shared_db database) {
    dao::paged_string strings;
    require(count(database) == 0 && database->set("raw", "opaque") &&
        count(database) == 1 && database->set("raw", "replacement") &&
        count(database) == 1, "count raw overwrite");
    require(database->del("raw") && !database->del("raw") &&
        count(database) == 0, "count raw missing delete");
    require(database->replica_set("replica", "x") &&
        database->replica_set("replica", "y") && count(database) == 1 &&
        database->replica_del("replica") &&
        database->replica_del("replica") && count(database) == 0,
        "count legacy replica overwrite/delete");
    require(strings.set(database, "", std::string(70000, 'x')) &&
        count(database) == 1, "payload pages counted as keys");
    dao::json_key keys;
    require(keys.set_expiry(database, "", 4102444800123LL, 0) &&
        count(database) == 1, "TTL changed root count");
    auto workspace = std::make_shared<page_workspace>(database);
    shared_db staged = workspace;
    require(strings.set(staged, "new", "v") && count(staged) == 2 &&
        strings.set(staged, "new", "v2") && count(staged) == 2 &&
        staged->del("") && count(staged) == 1 &&
        strings.set(staged, "", "reborn") && count(staged) == 2 &&
        staged->del("new") && count(staged) == 1 &&
        count(database) == 1, "staged root count history");
    require(workspace->commit() == db::batch_status::ok &&
        count(database) == 1 && enumerated(database) == 1,
        "workspace count commit");
    workspace.reset(); staged.reset();
    {
        auto pending = std::make_shared<page_workspace>(database);
        shared_db view = pending;
        require(view->set("discarded", "raw") && count(view) == 2,
            "workspace raw count");
    }
    require(count(database) == 1, "workspace discard changed source count");
    require(database->del("") && count(database) == 0, "count cleanup");
    string_root expired;
    expired.length = 7; expired.inline_value = "expired";
    expired.expires_at_ms = 1;
    std::string bytes, value;
    require(string_page_codec::encode_root(expired, bytes) ==
        string_page_status::ok && database->set("expired", bytes) &&
        count(database) == 1, "count filtered a lazy-expired root");
    require(!strings.get(database, "expired", value) &&
        strings.status() == dao::dao_base::read_status::missing &&
        count(database) == 0, "expired root cleanup count");
}
static void faults(bool tbb) {
    auto database = memory(tbb);
    dao::paged_string data;
    require(data.set(database, "old", "before"), "count fault seed");
    const std::map<std::string, std::string> values = {
        {"old", "after"}, {"new-a", std::string(70000, 'a')},
        {"new-b", std::string(70000, 'b')}};
    bool applied;
    budget = LONG_MAX;
    require(data.mset(database, values, false, applied), "count fault control");
    const auto allocations = LONG_MAX - budget; budget = -1;
    for (long position = 0; position <= allocations; ++position) {
        database->del("new-a"); database->del("new-b");
        require(data.set(database, "old", "before"), "count fault reset");
        bool ok = false;
        budget = position;
        try { ok = data.mset(database, values, false, applied); }
        catch (const std::bad_alloc&) {}
        budget = -1;
        require(count(database) == (ok ? 3 : 1) &&
            count(database) == enumerated(database), "count drift after OOM");
    }
    std::cout << "Root count allocation positions: " << allocations + 1
        << '\n';
}
static void concurrent(shared_db database, unsigned iterations = 100) {
    std::vector<std::thread> workers;
    for (unsigned worker = 0; worker < 4; ++worker)
        workers.emplace_back([&, worker] {
            for (unsigned i = 0; i < iterations; ++i) {
                const auto key = std::to_string(worker) + ':' +
                    std::to_string(i);
                require(database->set(key, "raw") &&
                    database->set(key, "updated"), "count concurrent write");
            }
        });
    for (unsigned i = 0; i < 100; ++i)
        require(count(database) <= 4 * iterations, "count concurrent overflow");
    for (auto& worker : workers) worker.join();
    require(count(database) == 4 * iterations &&
        enumerated(database) == 4 * iterations, "count concurrent total");
    for (unsigned worker = 0; worker < 4; ++worker)
        for (unsigned i = 0; i < iterations; ++i)
            require(database->del(std::to_string(worker) + ':' +
                std::to_string(i)), "count concurrent cleanup");
    require(count(database) == 0, "count concurrent cleanup total");
}
class no_reads : public mdb {
public:
    no_reads() : mdb(4) {}
    bool reject = false;
protected:
    bool dbget(const std::string& key, std::string& out) override {
        require(!reject, "DBSIZE read a root"); return mdb::dbget(key, out);
    }
    scan_status dbscan_keys(unsigned long long, size_t,
        std::vector<std::string>&, unsigned long long&) override {
        require(false, "DBSIZE scanned keys"); return scan_status::error;
    }
};
static void no_io() {
    auto source = std::make_shared<no_reads>();
    shared_db database = source;
    require(database->open("") && database->set("key", "raw"),
        "no-read count setup");
    source->reject = true;
    budget = 0;
    uint64_t value;
    const auto status = database->key_count(value);
    budget = -1;
    require(status == db::count_status::ok && value == 1,
        "root count required allocation");
}
static shared_db backend(const std::string& name) {
    if (name == "rdb") return db::create_rdb();
    if (name == "wdb") return db::create_wdb();
    return db::create_mdb_tbb(4);
}
static void disk(const std::string& name, const char* path, bool reopen) {
    auto database = backend(name);
    require(database && database->open(path), "count disk open");
    if (reopen) {
        require(count(database) == 3 && enumerated(database) == 3,
            "count reopen differs from roots");
        require(database->del("persist") && count(database) == 2 &&
            database->set("persist", "changed") && count(database) == 3,
            "count reopen mutation");
        return;
    }
    basic(database); concurrent(database, 10);
    dao::paged_string data;
    require(data.set(database, "persist", std::string(70000, 'x')) &&
        database->set("opaque", "not a record") &&
        database->set("", "empty-key") && count(database) == 3,
        "count persistent setup");
}
class rejected : public mdb {
public:
    rejected() : mdb(4) {}
    bool fail = false;
protected:
    batch_status dbapply_pages(const std::vector<batch_entry>& roots,
        const std::vector<page_change>& pages) override {
        return fail ? batch_status::error : mdb::dbapply_pages(roots, pages);
    }
};
static void commit_failure() {
    auto source = std::make_shared<rejected>();
    shared_db database = source;
    require(database->open(""), "count reject source");
    auto pending = std::make_shared<page_workspace>(database);
    shared_db staged = pending;
    dao::paged_string data;
    require(data.set(staged, "new", "v") && count(staged) == 1 &&
        count(database) == 0, "count rejected staging");
    source->fail = true;
    require(pending->commit() == db::batch_status::error &&
        count(database) == 0 && count(staged) == 1,
        "failed workspace commit changed count");
    source->fail = false;
    require(pending->commit() == db::batch_status::ok && count(database) == 1,
        "workspace retry double-counted root");
}
static void cross_store(const std::string& name, const char* paths) {
    auto database = backend(name);
    require(database && database->open(paths), "cross-store count open");
    std::string first = "cross:0", second;
    const auto shard = acl_hash_crc32(first.data(), first.size()) % 2;
    for (unsigned i = 1; second.empty(); ++i) {
        const auto candidate = "cross:" + std::to_string(i);
        if (acl_hash_crc32(candidate.data(), candidate.size()) % 2 != shard)
            second = candidate;
    }
    require(database->set(first, "a") && database->set(second, "b") &&
        count(database) == 2, "cross-store root aggregation");
    dao::paged_string data;
    bool applied;
    require(!data.mset(database, {{first, "new"}, {second, "new"}}, false,
        applied) && count(database) == 2 && enumerated(database) == 2,
        "unsupported cross-store commit changed count");
    std::string value;
    require(database->get(first, value) && value == "a" &&
        database->get(second, value) && value == "b",
        "cross-store count test changed roots");
}
int main(int argc, char** argv) {
    acl::acl_cpp_init();
    if (argc >= 3) {
        if (argc >= 4 && std::string(argv[3]) == "--cross-store")
            cross_store(argv[1], argv[2]);
        else disk(argv[1], argv[2], argc >= 4);
        return 0;
    }
    no_io(); commit_failure();
    for (bool tbb : {false, true}) {
#ifndef HAS_TBB
        if (tbb) continue;
#endif
        basic(memory(tbb)); faults(tbb); concurrent(memory(tbb));
    }
    std::cout << "Stored root count checks passed\n";
}
