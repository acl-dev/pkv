#include "stdafx.h"
#include "dao/paged_string.h"
#include "dao/json/json_string.h"
#include "db/mdb/mdb.h"
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <new>
#include <thread>

static thread_local long budget = -1;
static thread_local size_t largest = SIZE_MAX;
void* operator new(std::size_t size) {
    if (!budget || size > largest) throw std::bad_alloc();
    if (budget > 0) --budget;
    if (void* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { ::operator delete(p); }
#if __cplusplus >= 201402L
void operator delete(void* p, std::size_t) noexcept { ::operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept { ::operator delete(p); }
#endif
using namespace pkv;
static const int64_t future = 4102444800123LL;
static void require(bool ok, const char* message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}
static shared_db memory(bool tbb) {
    auto db = tbb ? db::create_mdb_tbb(16) : db::create_mdb(16);
    require(db && db->open(""), "MSETEX DB open"); return db;
}
static int64_t expiry(shared_db& db, const std::string& key) {
    std::string bytes;
    record_header header;
    require(db->get(key, bytes) && db->value_codec().stored_header(bytes,
        header) == codec_status::ok, "MSETEX expiry metadata");
    return header.expires_at_ms;
}
static void seed(dao::string& data, shared_db& db, const std::string& key,
    const std::string& value, int64_t deadline) {
    dao::set_options options; options.expires_at_ms = deadline;
    dao::set_result out;
    require(data.set(db, key, value, options, out) && out.applied,
        "MSETEX seed");
}
static void semantics(bool tbb, bool legacy) {
    auto db = memory(tbb);
    dao::paged_string native;
    dao::json_string json;
    dao::string& data = legacy ? static_cast<dao::string&>(json) : native;
    seed(data, db, "a", "old-a", future);
    seed(data, db, "b", "old-b", future + 100);
    const std::string a(70000, 'a'), b(40000, 'b');
    dao::string_assignments values = {{"a", &a}, {"b", &b}};
    dao::set_options options;
    options.when = dao::set_options::only_missing;
    options.expires_at_ms = future + 1000;
    bool applied;
    std::string before, after;
    require(db->get("a", before) && data.msetex(db, values, options, applied)
        && !applied && db->get("a", after) && before == after,
        "NX changed existing batch");
    options.when = dao::set_options::only_existing;
    values["missing"] = &a;
    require(data.msetex(db, values, options, applied) && !applied &&
        expiry(db, "a") == future, "XX partially updated batch");
    values.erase("missing"); options.keep_ttl = true;
    options.expires_at_ms = -1;
    require(data.msetex(db, values, options, applied) && applied &&
        expiry(db, "a") == future && expiry(db, "b") == future + 100,
        "KEEPTTL lost per-key deadlines");
    std::vector<dao::string_result> out;
    require(data.mget(db, {"a", "b"}, out) && out[0].value == a &&
        out[1].value == b, "MSETEX payload mismatch");
    options.when = dao::set_options::any; options.keep_ttl = false;
    require(data.msetex(db, values, options, applied) && applied &&
        expiry(db, "a") == -1 && expiry(db, "b") == -1,
        "MSETEX retained unrequested TTL");
    options.expires_at_ms = 1;
    require(data.msetex(db, values, options, applied) && applied &&
        expiry(db, "a") == 1 && expiry(db, "b") == 1,
        "MSETEX absolute past deadline");
    options.when = dao::set_options::only_missing;
    options.expires_at_ms = future;
    require(data.msetex(db, values, options, applied) && applied &&
        expiry(db, "a") == future, "NX treated expired roots as live");
}
static void legacy_native(bool tbb) {
    auto db = memory(tbb);
    dao::json_string json; dao::paged_string data;
    const std::string old(70000, 'o'), replacement(40000, 'r');
    seed(json, db, "a", old, future);
    seed(json, db, "b", old, future + 100);
    std::string before_a, before_b, after;
    require(db->get("a", before_a) && db->get("b", before_b),
        "legacy native snapshot");
    const dao::string_assignments values = {{"a", &replacement},
        {"b", &replacement}};
    dao::set_options options;
    options.when = dao::set_options::only_missing;
    bool applied;
    require(data.msetex(db, values, options, applied) && !applied &&
        db->get("a", after) && after == before_a &&
        db->get("b", after) && after == before_b,
        "native NX migrated live legacy bytes");
    options.when = dao::set_options::only_existing;
    options.keep_ttl = true;
    require(data.msetex(db, values, options, applied) && applied &&
        db->get("a", after) && after.compare(0, 3, "PSR") == 0 &&
        expiry(db, "a") == future && expiry(db, "b") == future + 100,
        "native batch did not import legacy with separate TTLs");
    std::vector<dao::string_result> out;
    require(data.mget(db, {"a", "b"}, out) &&
        out[0].value == replacement && out[1].value == replacement,
        "legacy native batch payload");
}
class tracked_db : public mdb {
public:
    tracked_db() : mdb(4) {}
    size_t commits = 0;
    bool reject = false;
protected:
    batch_status dbapply_pages(const std::vector<batch_entry>& roots,
        const std::vector<page_change>& pages) override {
        ++commits;
        for (const auto& page : pages)
            require(page.value.size() <= 16384, "unbounded MSETEX page");
        if (reject) return batch_status::error;
        return mdb::dbapply_pages(roots, pages);
    }
};
static void bounded() {
    auto spy = std::make_shared<tracked_db>(); shared_db db = spy;
    require(db->open(""), "MSETEX spy open");
    dao::paged_string data;
    const std::string a(1024 * 1024, 'a'), b(1024 * 1024, 'b');
    dao::string_assignments values = {{"a", &a}, {"b", &b}};
    dao::set_options options; options.expires_at_ms = future;
    bool applied;
    largest = 65536;
    const bool ok = data.msetex(db, values, options, applied);
    largest = SIZE_MAX;
    require(ok && applied && spy->commits == 1,
        "MSETEX copied large inputs or split commit");
    spy->commits = 0; options.when = dao::set_options::only_missing;
    require(data.msetex(db, values, options, applied) && !applied &&
        !spy->commits, "failed NX committed changes");
    spy->reject = true; options.when = dao::set_options::any;
    options.expires_at_ms = future + 100;
    require(!data.msetex(db, values, options, applied) && !applied,
        "rejected MSETEX reported success");
    spy->reject = false;
    require(expiry(db, "a") == future && expiry(db, "b") == future,
        "rejected MSETEX changed TTL");
}
static void coherent(bool tbb) {
    auto db = memory(tbb); dao::paged_string data;
    const std::string initial = "initial";
    bool applied;
    require(data.msetex(db, {{"a", &initial}, {"b", &initial}},
        dao::set_options(), applied) && applied, "coherence seed");
    std::atomic<unsigned> done(0);
    std::vector<std::thread> workers;
    for (unsigned t = 0; t < 4; ++t) workers.emplace_back([&, t] {
        dao::paged_string local;
        for (unsigned i = 0; i < 50; ++i) {
            const auto token = std::to_string(t) + ':' + std::to_string(i);
            const dao::string_assignments values = {{"a", &token},
                {"b", &token}};
            bool changed;
            dao::set_options options;
            options.expires_at_ms = future + t * 100 + i;
            require(local.msetex(db, values, options, changed) && changed,
                "concurrent MSETEX failed");
        }
        ++done;
    });
    do {
        std::vector<dao::string_result> out;
        require(data.mget(db, {"a", "b"}, out) && out[0].found &&
            out[1].found && out[0].value == out[1].value,
            "MSETEX exposed partial payload batch");
    } while (done != 4);
    for (auto& worker : workers) worker.join();
}
static void rollback(bool tbb, bool legacy) {
    const std::string old(20000, 'o'), replacement(40000, 'r');
    unsigned failures = 0; bool success = false;
    for (long limit = 0; limit < 1200; ++limit) {
        auto db = memory(tbb);
        dao::paged_string data; dao::json_string json;
        dao::string& source = legacy ? static_cast<dao::string&>(json) : data;
        seed(source, db, "a", old, future);
        seed(source, db, "b", old, future + 100);
        dao::set_options options;
        options.when = dao::set_options::only_existing;
        options.keep_ttl = true;
        const dao::string_assignments values = {{"a", &replacement},
            {"b", &replacement}};
        bool applied = false, ok = false;
        budget = limit;
        try { ok = data.msetex(db, values, options, applied); }
        catch (const std::bad_alloc&) {}
        budget = -1;
        if (ok) {
            require(applied, "MSETEX allocation success flag");
            success = true; break;
        }
        ++failures;
        std::string a, b;
        require(!applied && expiry(db, "a") == future &&
            expiry(db, "b") == future + 100 && data.get(db, "a", a) &&
            data.get(db, "b", b) && a == old && b == old,
            "failed MSETEX changed values/TTLs");
    }
    require(success && failures > 10, "MSETEX rollback sweep incomplete");
}
static shared_db backend(const std::string& name) {
    if (name == "rdb") return db::create_rdb();
    if (name == "wdb") return db::create_wdb();
    return db::create_mdb_tbb(4);
}
static void persistent(const std::string& name, const char* path, bool reopen) {
    auto db = backend(name);
    require(db && db->open(path), "MSETEX backend open");
    dao::paged_string data;
    const std::string a(40000, 'a'), b(60000, 'b');
    if (!reopen) {
        bool applied;
        dao::set_options options; options.expires_at_ms = future;
        require(data.msetex(db, {{"a", &a}, {"b", &b}}, options, applied)
            && applied, "persistent MSETEX");
    }
    std::string first, second;
    require(data.get(db, "a", first) && data.get(db, "b", second) &&
        first == a && second == b && expiry(db, "a") == future &&
        expiry(db, "b") == future, "reopened MSETEX lost data/TTL");
}
static void cross_store(const std::string& name, const char* paths) {
    auto db = backend(name);
    require(db && db->open(paths), "MSETEX cross-store open");
    const std::string a = "key:0", old = "old", replacement = "new";
    std::string b;
    const auto shard = acl_hash_crc32(a.data(), a.size()) % 2;
    for (unsigned i = 1; b.empty(); ++i) {
        const auto key = "key:" + std::to_string(i);
        if (acl_hash_crc32(key.data(), key.size()) % 2 != shard) b = key;
    }
    dao::paged_string data;
    seed(data, db, a, old, future); seed(data, db, b, old, future + 100);
    const dao::string_assignments values = {{a, &replacement},
        {b, &replacement}};
    dao::set_options options; bool applied;
    require(!data.msetex(db, values, options, applied) && !applied &&
        data.status() == dao::dao_base::read_status::unsupported_operation,
        "cross-store MSETEX partially accepted");
    std::string first, second;
    require(data.get(db, a, first) && data.get(db, b, second) && first == old
        && second == old && expiry(db, a) == future &&
        expiry(db, b) == future + 100, "cross-store MSETEX changed data");
    options.when = dao::set_options::only_missing;
    require(data.msetex(db, values, options, applied) && !applied,
        "cross-store NX no-op failed");
}
int main(int argc, char** argv) {
    acl::acl_cpp_init();
    if (argc >= 3) {
        const std::string mode = argc == 4 ? argv[3] : "";
        if (mode == "--cross-store") cross_store(argv[1], argv[2]);
        else persistent(argv[1], argv[2], mode == "--reopen");
        return 0;
    }
    bounded();
    for (bool tbb : {false, true}) {
#ifndef HAS_TBB
        if (tbb) continue;
#endif
        semantics(tbb, false); semantics(tbb, true); coherent(tbb);
        legacy_native(tbb);
        rollback(tbb, false); rollback(tbb, true);
    }
    std::cout << "MSETEX batch, TTL and rollback checks passed\n";
}
