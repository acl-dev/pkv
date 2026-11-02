#include "stdafx.h"
#include "common/redis_hll.h"
#include "dao/paged_string.h"
#include "db/mdb/mdb.h"
#include <cstdlib>
#include <iostream>
#include <new>

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
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
#endif
using namespace pkv;
static void require(bool ok, const char* message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}
class tracked_db : public mdb {
public:
    tracked_db() : mdb(4) {}
    size_t reads = 0;
    bool reject = false;
protected:
    batch_status dbapply_pages(const std::vector<batch_entry>& roots,
        const std::vector<page_change>& pages) override {
        return reject ? batch_status::error : mdb::dbapply_pages(roots, pages);
    }
    lookup_status dblookup_page(const std::string& key, uint64_t id,
        std::string& value) override {
        ++reads; return mdb::dblookup_page(key, id, value);
    }
};
static void bounds() {
    auto spy = std::make_shared<tracked_db>();
    shared_db database = spy;
    require(database->open(""), "bounded HLL DB open");
    dao::paged_string data;
    auto huge = hll::empty();
    huge.resize(4 * 1024 * 1024, '\0'); hll::cache(huge, 99);
    require(data.set(database, "huge", huge), "huge fake HLL seed");
    spy->reads = 0;
    long long count;
    const std::string element("new");
    largest = 65536;
    require(data.pfcount(database, {"huge"}, count) && count == 99,
        "cached PFCOUNT read the huge body");
    require(data.pfadd(database, "huge", {}, count) && !count,
        "empty PFADD read the huge body");
    require(!data.pfadd(database, "huge", {&element}, count) &&
        data.status() == dao::dao_base::read_status::corrupt_hll,
        "PFADD materialized a huge corrupt body");
    require(!data.pfcount(database, {"huge", "missing"}, count) &&
        data.status() == dao::dao_base::read_status::corrupt_hll,
        "union PFCOUNT materialized a huge corrupt body");
    require(!data.pfmerge(database, {"out", "huge"}) &&
        data.status() == dao::dao_base::read_status::corrupt_hll,
        "PFMERGE materialized a huge corrupt body");
    largest = SIZE_MAX;
    require(spy->reads == 5, "HLL header paths read extra native pages");
    auto value = hll::empty(); hll::invalidate(value);
    require(data.set(database, "small", value), "commit-failure seed");
    std::vector<watch_token> watched;
    require(database->watch({"small"}, watched) == db::batch_status::ok,
        "commit-failure watch");
    spy->reject = true;
    require(!data.pfadd(database, "small", {&element}, count) && !count &&
        !data.pfcount(database, {"small"}, count) && !count &&
        !data.pfmerge(database, {"small", "small"}),
        "rejected storage commit reported HLL success");
    spy->reject = false;
    std::string after;
    require(data.get(database, "small", after) && after == value &&
        watched[0]->valid(unix_time_ms()), "rejected commit changed HLL");
}
static void failures(bool tbb) {
    auto source = hll::empty();
    for (size_t i = 0; i < 20000; ++i) {
        bool changed;
        require(hll::add(source, std::to_string(i), changed), "HLL seed");
    }
    auto empty = hll::empty();
    require(hll::dense(empty), "dense empty seed"); hll::invalidate(empty);
    size_t failed = 0, succeeded = 0;
    const std::string element("element");
    const std::vector<const std::string*> elements{&element};
    const std::vector<std::string> single{"out"}, inputs{"out", "source"};
    for (int operation = 0; operation < 3; ++operation) {
        size_t failures = 0, successes = 0;
        for (long limit = 0; limit < 128; ++limit) {
            auto database = tbb ? db::create_mdb_tbb(4) : db::create_mdb(4);
            require(database && database->open(""), "fault HLL DB open");
            dao::paged_string data;
            require(data.set(database, "out", empty) &&
                data.set(database, "source", source), "fault data seed");
            std::string before, after, root;
            require(database->get("out", root), "fault root snapshot");
            std::vector<watch_token> tokens;
            require(database->watch({"out"}, tokens) == db::batch_status::ok,
                "fault watch setup");
            bool ok = false; long long result = 0;
            budget = limit;
            try {
                if (!operation) ok = data.pfadd(database, "out", elements,
                    result);
                else if (operation == 1)
                    ok = data.pfcount(database, single, result);
                else ok = data.pfmerge(database, inputs);
            } catch (const std::bad_alloc&) {}
            budget = -1;
            if (ok) {
                ++successes;
                require(!tokens[0]->valid(unix_time_ms()),
                    "successful HLL write did not invalidate WATCH");
            } else {
                ++failures;
                require(database->get("out", after) && after == root &&
                    data.get(database, "out", before) && before == empty &&
                    tokens[0]->valid(unix_time_ms()),
                    "failed HLL operation changed root, pages or WATCH");
            }
        }
        require(failures && successes, "HLL fault range missed a path");
        failed += failures; succeeded += successes;
    }
    std::cout << "HLL allocation budgets: " << (tbb ? "tbb" : "mdb")
        << " failed=" << failed << " succeeded=" << succeeded << '\n';
}
int main() {
    bounds(); failures(false);
#ifdef HAS_TBB
    failures(true);
#endif
    std::cout << "HLL bounded input reads and allocation rollback passed\n";
}
