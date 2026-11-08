#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#if defined(__aarch64__) && defined(__ARM_NEON)
#include <arm_neon.h>
#endif

namespace pkv {

struct bitmap_range {
    long long start = 0;
    long long end = -1;
    bool bits = false;
    bool end_given = false;
};

// Inclusive bit bounds. Unlike BITCOUNT, BITPOS normalizes reversed negative
// indexes before testing whether the range is empty.
inline bool bitmap_bounds(uint64_t length, const bitmap_range& range,
    bool count, uint64_t& first, uint64_t& last) {
    const auto size = static_cast<long long>(range.bits ? length * 8 : length);
    auto start = range.start;
    auto end = range.end;
    if (!length || (count && start < 0 && end < 0 && start > end))
        return false;
    if (start < 0) start += size;
    if (end < 0) end += size;
    if (start < 0) start = 0;
    if (end < 0) end = 0;
    if (end >= size) end = size - 1;
    if (start > end) return false;
    first = static_cast<uint64_t>(start) * (range.bits ? 1 : 8);
    last = static_cast<uint64_t>(end) * (range.bits ? 1 : 8) +
        (range.bits ? 0 : 7);
    return true;
}

inline unsigned bitmap_word_count(uint64_t word) {
#if defined(__GNUC__) || defined(__clang__)
    return static_cast<unsigned>(__builtin_popcountll(word));
#else
    unsigned count = 0;
    while (word) { word &= word - 1; ++count; }
    return count;
#endif
}

inline uint64_t bitmap_popcount(const char* bytes, size_t size) {
    uint64_t count = 0;
#if defined(__aarch64__) && defined(__ARM_NEON)
    while (size >= 64) {
        // Each lane counts at most 64 bits per block. Reduce before a
        // 16-bit lane could overflow, even for an all-ones input.
        const auto blocks = size > 32768 ? size_t(512) : size / 64;
        auto lanes = vdupq_n_u16(0);
        auto* data = reinterpret_cast<const uint8_t*>(bytes);
        for (size_t i = 0; i < blocks; ++i, data += 64) {
            const auto a = vcntq_u8(vld1q_u8(data));
            const auto b = vcntq_u8(vld1q_u8(data + 16));
            const auto c = vcntq_u8(vld1q_u8(data + 32));
            const auto d = vcntq_u8(vld1q_u8(data + 48));
            lanes = vpadalq_u8(lanes, vaddq_u8(vaddq_u8(a, b),
                vaddq_u8(c, d)));
        }
        count += vaddlvq_u16(lanes);
        bytes += blocks * 64;
        size -= blocks * 64;
    }
#endif
    while (size >= sizeof(uint64_t)) {
        uint64_t word;
        std::memcpy(&word, bytes, sizeof(word));
        count += bitmap_word_count(word);
        bytes += sizeof(word); size -= sizeof(word);
    }
    while (size--) count += bitmap_word_count(
        static_cast<unsigned char>(*bytes++));
    return count;
}

inline long long bitmap_position(const char* bytes, size_t size, bool bit) {
    size_t offset = 0;
#if defined(__aarch64__) && defined(__ARM_NEON)
    const auto fill = vdupq_n_u8(bit ? 0 : 255);
    while (size - offset >= 32) {
        const auto* data = reinterpret_cast<const uint8_t*>(bytes + offset);
        const auto first = veorq_u8(vld1q_u8(data), fill);
        const auto second = veorq_u8(vld1q_u8(data + 16), fill);
        if (vmaxvq_u8(vorrq_u8(first, second))) break;
        offset += 32;
    }
#endif
    const uint64_t absent = bit ? 0 : UINT64_MAX;
    while (size - offset >= sizeof(uint64_t)) {
        uint64_t word;
        std::memcpy(&word, bytes + offset, sizeof(word));
        if (word != absent) break;
        offset += sizeof(word);
    }
    for (; offset < size; ++offset) {
        const auto byte = static_cast<unsigned char>(bytes[offset]);
        if (byte == (bit ? 0 : 255)) continue;
        for (unsigned i = 0; i < 8; ++i)
            if (((byte >> (7 - i)) & 1) == static_cast<unsigned>(bit))
                return static_cast<long long>(offset * 8 + i);
    }
    return -1;
}

} // namespace pkv
