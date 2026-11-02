#include "redis_geo.h"
#include <algorithm>
#include <cmath>
#include <strings.h>

namespace pkv {
namespace geo {
namespace {
const double latitude_limit = 85.05112878;
const double scale = 67108864.0;
const double radians = 0.017453292519943295769236907684886;
const double earth_radius = 6372797.560856;
uint64_t interleave(uint32_t latitude, uint32_t longitude) {
    uint64_t bits = 0;
    for (unsigned i = 0; i < 32; ++i) {
        bits |= uint64_t((latitude >> i) & 1) << (2 * i);
        bits |= uint64_t((longitude >> i) & 1) << (2 * i + 1);
    }
    return bits;
}
uint64_t encode(const point& value, double latitude_range) {
    const auto latitude = static_cast<uint32_t>(
        (value.latitude + latitude_range) / (2 * latitude_range) * scale);
    const auto longitude = static_cast<uint32_t>(
        (value.longitude + 180) / 360 * scale);
    return interleave(latitude, longitude);
}
double center(uint32_t index, double limit) {
    const auto lower = -limit + (index * 1.0 / scale) * (2 * limit);
    const auto upper = -limit + ((index + 1) * 1.0 / scale) * (2 * limit);
    return std::max(-limit, std::min(limit, (lower + upper) / 2));
}
}
bool valid(const point& value) {
    return std::isfinite(value.longitude) && std::isfinite(value.latitude) &&
        value.longitude >= -180 && value.longitude <= 180 &&
        value.latitude >= -latitude_limit && value.latitude <= latitude_limit;
}
uint64_t encode(const point& value) { return encode(value, latitude_limit); }
point decode(double score) {
    uint64_t bits = 0;
    if (score >= 18446744073709551616.0) bits = UINT64_MAX;
    else if (score > 0) bits = static_cast<uint64_t>(score);
    uint32_t latitude = 0, longitude = 0;
    for (unsigned i = 0; i < 32; ++i) {
        latitude |= uint32_t((bits >> (2 * i)) & 1) << i;
        longitude |= uint32_t((bits >> (2 * i + 1)) & 1) << i;
    }
    point value;
    value.longitude = center(longitude, 180);
    value.latitude = center(latitude, latitude_limit);
    return value;
}
std::string hash(const point& value) {
    static const char alphabet[] = "0123456789bcdefghjkmnpqrstuvwxyz";
    const auto bits = encode(value, 90);
    std::string text(11, '0');
    for (unsigned i = 0; i < 10; ++i)
        text[i] = alphabet[(bits >> (47 - 5 * i)) & 31];
    return text;
}
double distance(const point& first, const point& second) {
    const auto longitude1 = first.longitude * radians;
    const auto longitude2 = second.longitude * radians;
    const auto v = std::sin((longitude2 - longitude1) / 2);
    if (v == 0) {
        const auto first_latitude = first.latitude * radians;
        const auto second_latitude = second.latitude * radians;
        return earth_radius * std::fabs(second_latitude - first_latitude);
    }
    const auto latitude1 = first.latitude * radians;
    const auto latitude2 = second.latitude * radians;
    const auto u = std::sin((latitude2 - latitude1) / 2);
    const auto a = u * u + std::cos(latitude1) * std::cos(latitude2) * v * v;
    return 2 * earth_radius * std::asin(std::sqrt(a));
}
std::string distance_text(double value) {
    // Distances are finite, nonnegative and below the Earth's circumference.
    const auto rounded = static_cast<uint64_t>(std::llrint(value * 10000));
    const auto fraction = std::to_string(rounded % 10000);
    return std::to_string(rounded / 10000) + "." +
        std::string(4 - fraction.size(), '0') + fraction;
}
double unit(const std::string& name) {
    if (!strcasecmp(name.c_str(), "m")) return 1;
    if (!strcasecmp(name.c_str(), "km")) return 1000;
    if (!strcasecmp(name.c_str(), "ft")) return 0.3048;
    if (!strcasecmp(name.c_str(), "mi")) return 1609.34;
    return -1;
}
} // namespace geo
} // namespace pkv
