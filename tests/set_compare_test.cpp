#include "stdafx.h"
#include "dao/paged_string.h"
#include "dao/json/json_string.h"
#include "db/mdb/mdb.h"
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
void operator delete[](void* p) noexcept { std::free(p); }
#if __cplusplus >= 201402L
void operator delete(void* p, std::size_t) noexcept { ::operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept { ::operator delete(p); }
#endif
using namespace pkv;
static void require(bool ok, const char* message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}
static shared_db memory(bool tbb) {
    auto db = tbb ? db::create_mdb_tbb(16) : db::create_mdb(16);
    require(db && db->open(""), "comparison DB open"); return db;
}
class tracked_db : public mdb {
public:
    tracked_db() : mdb(4) {}
    size_t reads = 0, writes = 0;
    bool broken = false, reject = false;
protected:
    lookup_status dblookup_page(const std::string& key, uint64_t id,
        std::string& out) override {
        ++reads;
        if (broken && id == 2) return lookup_status::missing;
        return mdb::dblookup_page(key, id, out);
    }
    batch_status dbapply_pages(const std::vector<batch_entry>& roots,
        const std::vector<page_change>& pages) override {
        writes += pages.size();
        if (reject) return batch_status::error;
        return mdb::dbapply_pages(roots, pages);
    }
};
static void locality() {
    auto spy = std::make_shared<tracked_db>();
    shared_db db = spy;
    require(db->open(""), "comparison spy open");
    dao::paged_string data;
    std::string value(4 * 1024 * 1024, 'x'), expected = value;
    require(data.set(db, "key", value), "comparison large seed");
    dao::set_options options;
    options.when = dao::set_options::only_different;
    options.comparison = &expected;
    dao::set_result out;
    spy->reads = spy->writes = 0;
    largest = 65536;
    const bool ok = data.set(db, "key", "replacement", options, out);
    largest = SIZE_MAX;
    require(ok && !out.applied && spy->reads == 256 && !spy->writes,
        "comparison materialized or rewrote large value");
    expected.pop_back(); spy->reads = 0;
    options.when = dao::set_options::only_equal;
    require(data.set(db, "key", "x", options, out) && !out.applied &&
        !spy->reads && !spy->writes, "length mismatch read payload pages");
    expected = value; expected[0] = 'y'; spy->reads = 0;
    require(data.set(db, "key", "x", options, out) && !out.applied &&
        spy->reads == 1 && !spy->writes, "comparison missed early exit");
    expected = value; spy->broken = true;
    require(!data.set(db, "key", "x", options, out) && !out.applied,
        "missing comparison page accepted");
    spy->broken = false; spy->reject = true;
    require(!data.set(db, "key", "x", options, out) && !out.applied,
        "rejected comparison commit reported success");
    spy->reject = false;
    std::string actual;
    require(data.get(db, "key", actual) && actual == value,
        "failed comparison changed original payload");
    expected = "different";
    options.return_old = true;
    spy->reads = spy->writes = 0;
    require(data.set(db, "key", "x", options, out) && !out.applied &&
        out.had_old && out.old_value == value && spy->reads == 256 &&
        !spy->writes, "GET comparison read payload twice");
}
static void legacy(bool tbb) {
    auto db = memory(tbb);
    dao::json_string json;
    dao::paged_string paged;
    std::string expected(70000, '\0'), before, after;
    require(json.set(db, "key", expected) && db->get("key", before),
        "legacy comparison seed");
    dao::set_options options;
    options.when = dao::set_options::only_different;
    options.comparison = &expected;
    dao::set_result out;
    require(paged.set(db, "key", "new", options, out) && !out.applied &&
        db->get("key", after) && before == after,
        "failed condition migrated legacy bytes");
    options.when = dao::set_options::only_equal;
    require(paged.set(db, "key", "new", options, out) && out.applied &&
        db->get("key", after) && after.compare(0, 3, "PSR") == 0,
        "successful comparison did not migrate legacy bytes");
    require(json.set(db, "json", expected), "legacy DAO seed");
    require(json.set(db, "json", "new", options, out) && out.applied,
        "legacy DAO comparison failed");
}
static void concurrent(bool tbb) {
    auto db = memory(tbb);
    dao::paged_string data;
    require(data.set(db, "count", "0"), "CAS counter seed");
    std::vector<std::thread> workers;
    for (unsigned t = 0; t < 4; ++t) workers.emplace_back([&] {
        dao::paged_string local;
        for (unsigned i = 0; i < 100; ++i) {
            for (;;) {
                std::string expected;
                require(local.get(db, "count", expected), "CAS read");
                const auto next = std::to_string(std::stoll(expected) + 1);
                dao::set_options options;
                options.when = dao::set_options::only_equal;
                options.comparison = &expected;
                dao::set_result out;
                require(local.set(db, "count", next, options, out), "CAS set");
                if (out.applied) break;
            }
        }
    });
    for (auto& worker : workers) worker.join();
    std::string actual;
    require(data.get(db, "count", actual) && actual == "400",
        "concurrent CAS lost updates");
}
static void rollback(bool tbb) {
    const std::string expected(40000, 'a'), replacement(60000, 'b');
    unsigned failures = 0;
    bool success = false;
    for (long limit = 0; limit < 1000; ++limit) {
        auto db = memory(tbb);
        dao::paged_string data;
        require(data.set(db, "key", expected), "CAS rollback seed");
        dao::set_options options;
        options.when = dao::set_options::only_equal;
        options.comparison = &expected;
        dao::set_result out;
        budget = limit;
        bool ok = false;
        try { ok = data.set(db, "key", replacement, options, out); }
        catch (const std::bad_alloc&) {}
        budget = -1;
        if (ok) {
            require(out.applied, "CAS success lost applied flag");
            success = true; break;
        }
        ++failures;
        std::string actual;
        require(!out.applied && data.get(db, "key", actual) &&
            actual == expected, "failed CAS changed pages");
    }
    require(success && failures > 10, "CAS rollback sweep incomplete");
}
int main() {
    acl::acl_cpp_init(); locality();
    for (bool tbb : {false, true}) {
#ifndef HAS_TBB
        if (tbb) continue;
#endif
        legacy(tbb); concurrent(tbb); rollback(tbb);
    }
    std::cout << "SET comparison, locality and rollback checks passed\n";
}
