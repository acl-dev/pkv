// Grid planning adapts Redis's BSD-licensed geohash helpers.
// See third_party/redis-geohash/LICENSE for copyright and license.
#include "geo_search.h"
#include <cmath>
#include <algorithm>

namespace pkv {
namespace geo {
namespace {
const double lat_limit = 85.05112878, earth = 6372797.560856;
const double radians = 0.017453292519943295769236907684886;
struct cell { uint32_t x, y; };
struct box { double left, bottom, right, top; };
uint64_t bits(const cell& value) {
    uint64_t result = 0;
    for (unsigned i = 0; i < 32; ++i) {
        result |= uint64_t((value.y >> i) & 1) << (2 * i);
        result |= uint64_t((value.x >> i) & 1) << (2 * i + 1);
    }
    return result;
}
box bounds(const cell& value, unsigned step) {
    const double scale = uint32_t(1) << step;
    return {-180 + (value.x * 1.0 / scale) * 360,
        -lat_limit + (value.y * 1.0 / scale) * (2 * lat_limit),
        -180 + ((value.x + 1) * 1.0 / scale) * 360,
        -lat_limit + ((value.y + 1) * 1.0 / scale) * (2 * lat_limit)};
}
std::array<cell, 9> neighbors(const point& value, unsigned step) {
    const auto scale = uint32_t(1) << step;
    const auto x = uint32_t((value.longitude + 180) / 360 * scale);
    const auto y = uint32_t((value.latitude + lat_limit) /
        (2 * lat_limit) * scale);
    const auto mask = scale - 1;
    // Axis movement wraps only that axis, retaining inclusive endpoint bits
    // on the other axis. Order also defines unsorted and COUNT ANY replies.
    return {{{x, y}, {x, (y + 1) & mask}, {x, (y - 1) & mask},
        {(x + 1) & mask, y}, {(x - 1) & mask, y},
        {(x + 1) & mask, (y + 1) & mask},
        {(x - 1) & mask, (y + 1) & mask},
        {(x + 1) & mask, (y - 1) & mask},
        {(x - 1) & mask, (y - 1) & mask}}};
}
box extent(const shape& area) {
    const auto h = area.conversion * (area.box ? area.height / 2 : area.radius);
    const auto w = area.conversion * (area.box ? area.width / 2 : area.radius);
    const auto latitude = h / earth / radians;
    const auto edge = area.center.latitude < 0 ?
        area.center.latitude - latitude : area.center.latitude + latitude;
    const auto longitude = w / earth / std::cos(edge * radians) / radians;
    return {area.center.longitude - longitude, area.center.latitude - latitude,
        area.center.longitude + longitude, area.center.latitude + latitude};
}
unsigned precision(const shape& area) {
    auto radius = area.box ? std::sqrt((area.width / 2) * (area.width / 2) +
        (area.height / 2) * (area.height / 2)) : area.radius;
    radius *= area.conversion;
    if (radius == 0) return 26;
    int step = 1;
    while (radius < 20037726.37) { radius *= 2; ++step; }
    step -= 2;
    if (std::fabs(area.center.latitude) > 66) --step;
    if (std::fabs(area.center.latitude) > 80) --step;
    return unsigned(std::max(1, std::min(26, step)));
}
}
std::array<score_range, 9> search_ranges(const shape& area) {
    const auto cover = extent(area);
    auto step = precision(area);
    auto cells = neighbors(area.center, step);
    if (step > 1 && (bounds(cells[1], step).top < cover.top ||
        bounds(cells[2], step).bottom > cover.bottom ||
        bounds(cells[3], step).right < cover.right ||
        bounds(cells[4], step).left > cover.left)) {
        --step; cells = neighbors(area.center, step);
    }
    const auto center = bounds(cells[0], step);
    const int dx[] = {0, 0, 0, 1, -1, 1, -1, 1, -1};
    const int dy[] = {0, 1, -1, 0, 0, 1, 1, -1, -1};
    std::array<score_range, 9> ranges{};
    unsigned last = 0;
    for (unsigned i = 0; i < cells.size(); ++i) {
        if (step >= 2 && ((dy[i] < 0 && center.bottom < cover.bottom) ||
            (dy[i] > 0 && center.top > cover.top) ||
            (dx[i] < 0 && center.left < cover.left) ||
            (dx[i] > 0 && center.right > cover.right))) continue;
        const auto prefix = bits(cells[i]);
        if (last && prefix == bits(cells[last])) continue;
        ranges[i].enabled = true;
        ranges[i].lower = double(prefix << (52 - 2 * step));
        ranges[i].upper = double((prefix + 1) << (52 - 2 * step));
        last = i;
    }
    return ranges;
}
bool contains(const shape& area, const point& value, double& meters) {
    if (area.box) {
        const auto latitude = value.latitude * radians;
        const auto center_latitude = area.center.latitude * radians;
        const auto vertical = earth * std::fabs(latitude - center_latitude);
        if (vertical > area.height * area.conversion / 2) return false;
        point along = area.center; along.latitude = value.latitude;
        if (distance(value, along) > area.width * area.conversion / 2)
            return false;
    }
    meters = distance(area.center, value);
    return area.box || !(meters > area.radius * area.conversion);
}
} // namespace geo
} // namespace pkv
