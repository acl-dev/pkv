// Optional external-reference validation; see doc/GEO_SEARCH.md.
#include "common/geo_search.h"
extern "C" {
#include <geohash_helper.h>
}
#include <random>
#include <iostream>
int main() {
    std::mt19937_64 random(77127);
    for (int i = 0; i < 100000; ++i) {
        pkv::geo::shape area;
        area.center.longitude = double(random() % 360000000) / 1000000 - 180;
        area.center.latitude = double(random() % 170000000) / 1000000 - 85;
        area.box = i % 2;
        area.radius = double(random() % 25000000) / (i % 5 ? 1 : 1000000.0);
        area.width = double(random() % 30000000);
        area.height = double(random() % 30000000);
        GeoShape input{};
        input.xy[0] = area.center.longitude;
        input.xy[1] = area.center.latitude;
        input.conversion = 1;
        input.type = area.box ? RECTANGLE_TYPE : CIRCULAR_TYPE;
        if (area.box) {
            input.t.r.width = area.width; input.t.r.height = area.height;
        }
        else input.t.radius = area.radius;
        auto ref = geohashCalculateAreasByShapeWGS84(&input);
        GeoHashBits all[] = {ref.hash, ref.neighbors.north, ref.neighbors.south,
            ref.neighbors.east, ref.neighbors.west, ref.neighbors.north_east,
            ref.neighbors.north_west, ref.neighbors.south_east,
            ref.neighbors.south_west};
        const auto ranges = pkv::geo::search_ranges(area);
        int last = 0;
        for (int j = 0; j < 9; ++j) {
            const bool enabled = (all[j].bits || all[j].step) &&
                !(last && all[last].bits == all[j].bits &&
                    all[last].step == all[j].step);
            if (ranges[j].enabled != enabled || (enabled &&
                (ranges[j].lower != double(all[j].bits <<
                    (52 - 2 * all[j].step)) ||
                 ranges[j].upper != double((all[j].bits + 1) <<
                    (52 - 2 * all[j].step))))) {
                std::cerr << "grid mismatch " << i << ' ' << j << '\n';
                return 1;
            }
            if (enabled) last = j;
        }
    }
    std::cout << "100000 reference grid plans matched\n";
}
