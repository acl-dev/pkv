#include "stdafx.h"
#include "common/redis_geo.h"
#include "dao/paged_zset.h"
#include "dao/json/json_zset.h"
#include "db/mdb/mdb.h"
#include "db/mdb/mdb_tbb.h"
#include "db/page_transaction.h"
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <random>

using namespace pkv;
static void require(bool ok, const char* reason) {
    if (!ok) { std::cerr << reason << '\n'; std::exit(1); }
}
static geo::point point(double longitude, double latitude) {
    geo::point value;
    value.longitude = longitude; value.latitude = latitude;
    return value;
}
template<class Backend> class bounded : public Backend {
public:
    bounded() : Backend(4) {}
    size_t rows = 0;
protected:
    db::batch_status dbapply_pages(const std::vector<db::batch_entry>& roots,
        const std::vector<page_change>& pages) override {
        for (const auto& root : roots)
            require(root.value.size() <= 16384, "unbounded GEO root");
        for (const auto& page : pages) {
            require(page.value.size() <= 16384, "unbounded GEO private page");
            ++rows;
        }
        return Backend::dbapply_pages(roots, pages);
    }
};
template<class Backend> static void storage() {
    auto source = std::make_shared<bounded<Backend>>();
    shared_db database = source;
    require(database->open(""), "GEO storage open");
    dao::paged_zset data;
    dao::json_zset legacy;
    dao::zadd_options options;
    dao::zadd_result result;
    const auto score = double(geo::encode(point(13.361389, 38.115556)));
    require(legacy.add(database, "legacy", {{"a", score}}, options, result),
        "GEO legacy seed");
    std::vector<zset_score> scores;
    require(data.scores(database, "legacy", {"a"}, scores) &&
        scores[0].found && geo::hash(geo::decode(scores[0].score)) ==
            "sqc8b49rny0", "GEO legacy import");
    std::vector<zset_member> values;
    const std::string large(40000, 'x');
    values.push_back({large, score});
    for (unsigned i = 0; i < 240; ++i)
        values.push_back({std::to_string(i), double(geo::encode(
            point(i / 2.0, i / 4.0)))});
    require(data.add(database, "native", values, options, result) &&
        result.added == 241 && source->rows > 1, "GEO paged members");
    require(data.scores(database, "native", {large}, scores) &&
        scores[0].found && scores[0].score == score, "GEO chunked member");
}
int main() {
    const auto palermo = point(13.361389, 38.115556);
    const auto catania = point(15.087269, 37.502669);
    require(geo::encode(palermo) == 3479099956230698ULL &&
        geo::encode(catania) == 3479447370796909ULL, "GEO score encoding");
    const auto first = geo::decode(double(geo::encode(palermo)));
    const auto second = geo::decode(double(geo::encode(catania)));
    require(geo::hash(first) == "sqc8b49rny0" &&
        geo::hash(second) == "sqdtr74hyu0", "standard GEOHASH conversion");
    require(geo::distance_text(geo::distance(first, second)) == "166274.1516",
        "GEO distance quantization");
    require(geo::distance_text(0) == "0.0000" &&
        geo::distance_text(1.23445) == "1.2344" &&
        geo::distance_text(1.23455) == "1.2346", "nearest-even distance text");
    require(!geo::valid(point(181, 0)) && !geo::valid(point(0, 86)) &&
        !geo::valid(point(std::numeric_limits<double>::infinity(), 0)),
        "invalid coordinate range");
    require(geo::unit("KM") == 1000 && geo::unit("mi") == 1609.34 &&
        geo::unit("meters") < 0, "GEO units");
    std::mt19937_64 random(77123);
    for (size_t i = 0; i < 100000; ++i) {
        const auto longitude = double(random() % 360000000) / 1000000 - 180;
        const auto latitude = double(random() % 170000000) / 1000000 - 85;
        const auto value = point(longitude, latitude);
        const auto decoded = geo::decode(double(geo::encode(value)));
        require(geo::valid(decoded) &&
            std::fabs(decoded.longitude - longitude) < 0.000003 &&
            std::fabs(decoded.latitude - latitude) < 0.000002,
            "GEO coordinate round trip");
        const auto other = geo::decode(double(random() % (1ULL << 52)));
        const auto distance = geo::distance(decoded, other);
        require(std::isfinite(distance) && distance >= 0 &&
            distance <= 20020735, "GEO spherical distance bounds");
    }
    for (double score : {-std::numeric_limits<double>::infinity(), -1.0,
        0.0, 4503599627370496.0, 18446744073709551616.0,
        std::numeric_limits<double>::infinity(),
        std::numeric_limits<double>::quiet_NaN()}) {
        const auto value = geo::decode(score);
        require(geo::valid(value) && geo::hash(value).size() == 11,
            "ordinary ZSet score decode not bounded");
    }
    for (auto value : {point(-180, -85.05112878), point(180, 85.05112878),
        point(180, -85.05112878), point(-180, 85.05112878)})
        require(geo::valid(geo::decode(double(geo::encode(value)))),
            "inclusive coordinate boundary");
    storage<mdb>();
#ifdef HAS_TBB
    storage<mdb_tbb>();
#endif
    std::cout << "GEO codec, 100000 round trips, distance and score bounds "
        "passed\n";
}
