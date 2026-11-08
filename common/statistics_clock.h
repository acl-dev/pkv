#pragma once
#include <cstdint>

namespace pkv {
namespace detail {
#if defined(__SIZEOF_INT128__)
// Reduce ticks -> microseconds once; usual Apple ratios become 1/divisor.
struct statistics_clock_ratio {
    uint64_t numerator = 0, denominator = 0;
    statistics_clock_ratio(uint32_t n, uint32_t d) noexcept {
        if (!n || !d) return;
        uint64_t a = n, b = uint64_t(d) * 1000;
        numerator = a; denominator = b;
        while (b) {
            const auto remainder = a % b;
            a = b; b = remainder;
        }
        numerator /= a; denominator /= a;
    }
    uint64_t microseconds(uint64_t ticks) const noexcept {
        return numerator == 1 ? ticks / denominator :
            static_cast<uint64_t>((__uint128_t(ticks) * numerator) /
                denominator);
    }
};
#endif
} // namespace detail
} // namespace pkv
