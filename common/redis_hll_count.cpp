#include "redis_hll.h"
#include <cmath>
#include <limits>

namespace pkv {
namespace hll {
namespace {
// Ertl's sigma/tau estimator for precision 14 (arXiv:1702.01284).
double sigma(double x) {
    if (x == 1) return std::numeric_limits<double>::infinity();
    double result = x, weight = 1, previous;
    do {
        x *= x; previous = result; result += x * weight; weight *= 2;
    } while (result != previous);
    return result;
}
double tau(double x) {
    if (x == 0 || x == 1) return 0;
    double result = 1 - x, weight = 1, previous;
    do {
        x = std::sqrt(x); previous = result; weight *= 0.5;
        result -= std::pow(1 - x, 2) * weight;
    } while (result != previous);
    return result / 3;
}
} // namespace
uint64_t count(const registers& values) {
    std::array<size_t, 64> histogram{};
    for (auto value : values) ++histogram[value];
    const double size = registers_size;
    double harmonic = size * tau((size - histogram[51]) / size);
    for (size_t i = 50; i > 0; --i)
        harmonic = (harmonic + histogram[i]) * 0.5;
    harmonic += size * sigma(histogram[0] / size);
    const double estimate = 0.721347520444481703680 * size * size / harmonic;
    if (!std::isfinite(estimate) || estimate >= std::ldexp(1.0, 63))
        return uint64_t(1) << 63;
    return static_cast<uint64_t>(std::llround(estimate));
}
} // namespace hll
} // namespace pkv
