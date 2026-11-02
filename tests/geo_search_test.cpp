#include "stdafx.h"
#include "common/geo_search.h"
#include "dao/paged_zset.h"
#include "db/mdb/mdb.h"
#include "db/mdb/mdb_tbb.h"
#include "db/page_workspace.h"
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <random>

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
static void require(bool ok, const char* reason) {
    if (!ok) { std::cerr << reason << '\n'; std::exit(1); }
}
static geo::point point(double x, double y) {
    geo::point value; value.longitude = x; value.latitude = y; return value;
}
template<class Backend> class tracked : public Backend {
public:
    tracked() : Backend(8) {}
    size_t reads = 0;
    bool reject = false, read_error = false;
protected:
    db::lookup_status dblookup_page(const std::string& owner, uint64_t id,
        std::string& out) override {
        ++reads;
        if (read_error) return db::lookup_status::error;
        return Backend::dblookup_page(owner, id, out);
    }
    db::batch_status dbapply_pages(const std::vector<db::batch_entry>& roots,
        const std::vector<page_change>& pages) override {
        if (reject) return db::batch_status::error;
        for (const auto& root : roots)
            require(root.value.size() <= 16384, "GEO search large root");
        for (const auto& page : pages)
            require(page.value.size() <= 16384, "GEO search large page");
        return Backend::dbapply_pages(roots, pages);
    }
};
static bool world(zset_store&, geo::search_options& options) {
    options.area.radius = std::numeric_limits<double>::infinity();
    return true;
}
static void geometry() {
    std::mt19937_64 random(7219);
    for (size_t i = 0; i < 100000; ++i) {
        auto value = point(double(random() % 36000000) / 100000 - 180,
            double(random() % 17000000) / 100000 - 85);
        const auto decoded = geo::decode(double(geo::encode(value)));
        require(geo::distance(decoded, decoded) == 0,
            "FMA produced nonzero self distance");
        geo::shape area; area.center = decoded; area.box = true;
        double meters = -1;
        require(geo::contains(area, decoded, meters) && meters == 0,
            "zero-size box omitted its own center");
        area.box = false;
        bool covered = false;
        const auto score = double(geo::encode(decoded));
        for (const auto& range : geo::search_ranges(area)) {
            if (!range.enabled) continue;
            require(range.lower < range.upper, "empty grid interval");
            if (score >= range.lower && score < range.upper) covered = true;
        }
        require(covered, "zero-radius grid omitted its own cell");
    }
}
template<class Backend> static void selective() {
    auto source = std::make_shared<tracked<Backend>>();
    shared_db database = source;
    require(database->open(""), "GEO indexed open");
    dao::paged_zset data;
    dao::zadd_options add;
    dao::zadd_result result;
    std::vector<zset_member> values;
    const auto center = point(13.361389, 38.115556);
    values.push_back({"center", double(geo::encode(center))});
    std::mt19937_64 random(7829);
    for (size_t i = 1; i < 4096; ++i) {
        const auto value = point(double(random() % 36000000) / 100000 - 180,
            double(random() % 17000000) / 100000 - 85);
        values.push_back({std::to_string(i), double(geo::encode(value))});
    }
    require(data.add(database, "source", values, add, result), "GEO seed");
    geo::search_options options;
    std::vector<geo::match> out;
    long long stored;
    source->reads = 0;
    require(data.geo_search(database, "source", nullptr,
        [&](zset_store&, geo::search_options& parsed) {
        parsed.area.center = center; parsed.area.radius = 1; return true;
    }, options, out, stored) && out.size() == 1 &&
        out[0].member == "center" && source->reads < 1024,
        "small GEO search scanned whole score index");
    const auto local_reads = source->reads;
    source->reads = 0;
    require(data.geo_search(database, "source", nullptr,
        [&](zset_store& store, geo::search_options& parsed) {
        world(store, parsed); parsed.count = 1; parsed.any = true; return true;
    }, options, out, stored) && out.size() == 1 && source->reads < 1024,
        "COUNT ANY did not stop the cursor");
    const auto any_reads = source->reads;
    out.clear(); out.shrink_to_fit();
    largest = 16384;
    const auto ok = data.geo_search(database, "source", nullptr,
        [&](zset_store& store, geo::search_options& parsed) {
        world(store, parsed); parsed.count = 1; return true;
    }, options, out, stored);
    largest = SIZE_MAX;
    require(ok && out.size() == 1 && options.order == 1,
        "COUNT retained all matches or failed default ASC");
    std::cout << "GEO index reads: " << database->get_dbtype() << " local="
        << local_reads << " any=" << any_reads << " elements=4096\n";
    const std::string large(40000, 'x'), target("large-result");
    require(data.add(database, "large", {{large, values[0].score}}, add,
        result) && data.geo_search(database, "large", &target, world, options,
        out, stored) && stored == 1 && out[0].member == large,
        "GEO search failed chunked member storage");
}
template<class Backend> static void faults() {
    size_t failures = 0, successes = 0;
    for (long allowance = 0; allowance < 512; ++allowance) {
        auto source = std::make_shared<tracked<Backend>>();
        shared_db database = source;
        require(database->open(""), "GEO fault open");
        dao::paged_zset data;
        dao::zadd_options add;
        dao::zadd_result result;
        const auto score = double(geo::encode(point(1, 2)));
        require(data.add(database, "source", {{"a", score}, {"b", score + 1}},
            add, result) && data.add(database, "destination", {{"old", 12}},
            add, result), "GEO fault seed");
        std::string before, after;
        require(database->get("destination", before), "GEO destination root");
        std::vector<watch_token> tokens;
        require(database->watch({"destination"}, tokens) ==
            db::batch_status::ok,
            "GEO destination watch");
        const std::string destination("destination");
        geo::search_options options;
        std::vector<geo::match> out;
        long long stored;
        budget = allowance;
        const auto ok = data.geo_search(database, "source", &destination,
            world, options, out, stored);
        budget = -1;
        if (ok) { ++successes; require(stored == 2, "GEO successful count"); }
        else {
            ++failures;
            require(out.empty() && !stored &&
                database->get(destination, after) && after == before &&
                tokens[0]->valid(unix_time_ms()), "failed GEO store published");
            std::vector<zset_member> actual;
            require(data.range(database, destination, 0, -1, false, actual) &&
                actual.size() == 1 && actual[0].member == "old" &&
                actual[0].score == 12, "failed GEO store changed pages");
        }
    }
    require(failures && successes, "GEO fault budgets incomplete");
    std::cout << "GEO store budgets: failed=" << failures << " succeeded="
        << successes << '\n';
}
template<class Backend> static void storage_errors() {
    auto source = std::make_shared<tracked<Backend>>();
    shared_db database = source;
    require(database->open(""), "GEO rejection open");
    dao::paged_zset data;
    dao::zadd_options add;
    dao::zadd_result result;
    const auto score = double(geo::encode(point(1, 2)));
    require(data.add(database, "source", {{"a", score}}, add, result),
        "GEO rejection seed");
    const std::string target("target");
    geo::search_options options;
    std::vector<geo::match> out;
    long long stored;
    source->reject = true;
    require(!data.geo_search(database, "source", &target, world, options,
        out, stored) && out.empty() && !stored && data.status() ==
        dao::dao_base::read_status::storage_error, "GEO commit failure hidden");
    source->reject = false; source->read_error = true;
    require(!data.geo_search(database, "source", nullptr, world, options,
        out, stored) && out.empty() && !stored && data.status() ==
        dao::dao_base::read_status::storage_error, "GEO read failure hidden");
    source->read_error = false;
    std::string bytes;
    require(!database->get(target, bytes), "failed GEO created destination");
}
int main() {
    geometry(); selective<mdb>(); faults<mdb>(); storage_errors<mdb>();
#ifdef HAS_TBB
    selective<mdb_tbb>(); faults<mdb_tbb>(); storage_errors<mdb_tbb>();
#endif
    std::cout << "GEO search bounded cursor, top-k and rollback passed\n";
}
