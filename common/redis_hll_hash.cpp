#include "redis_hll.h"

namespace pkv {
namespace hll {
// MurmurHash64A's endian-neutral arithmetic, with Redis's HLL seed.
uint64_t hash(const std::string& value) {
    const uint64_t multiplier = 0xc6a4a7935bd1e995ULL;
    uint64_t result = 0xadc83b19ULL ^ (value.size() * multiplier);
    size_t offset = 0;
    while (value.size() - offset >= 8) {
        uint64_t word = 0;
        for (size_t i = 0; i < 8; ++i) word |= static_cast<uint64_t>(
            static_cast<uint8_t>(value[offset + i])) << (8 * i);
        word *= multiplier; word ^= word >> 47; word *= multiplier;
        result ^= word; result *= multiplier; offset += 8;
    }
    if (offset < value.size()) {
        uint64_t tail = 0;
        for (size_t i = 0; offset + i < value.size(); ++i)
            tail |= static_cast<uint64_t>(static_cast<uint8_t>(
                value[offset + i])) << (8 * i);
        result ^= tail; result *= multiplier;
    }
    result ^= result >> 47; result *= multiplier; result ^= result >> 47;
    return result;
}
bool add(std::string& value, const std::string& element, bool& changed) {
    const auto bits = hash(element);
    auto frequency_bits = (bits >> 14) | (uint64_t(1) << 50);
    uint8_t frequency = 1;
    while (!(frequency_bits & 1)) { ++frequency; frequency_bits >>= 1; }
    return set(value, bits & (registers_size - 1), frequency, changed);
}
} // namespace hll
} // namespace pkv
