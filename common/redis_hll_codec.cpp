#include "redis_hll.h"
#include <algorithm>
#if defined(__aarch64__) && defined(__ARM_NEON)
#include <arm_neon.h>
#endif

namespace pkv {
namespace hll {
namespace {
void merge_dense(const uint8_t* packed, registers& into) {
    size_t i = 0;
#if defined(__aarch64__) && defined(__ARM_NEON)
    const uint8x16_t low_index = {0, 0, 1, 2, 3, 3, 4, 5,
        6, 6, 7, 8, 9, 9, 10, 11};
    const uint8x16_t high_index = {255, 1, 2, 255, 255, 4, 5, 255,
        255, 7, 8, 255, 255, 10, 11, 255};
    const int8x16_t low_shift = {0, -6, -4, -2, 0, -6, -4, -2,
        0, -6, -4, -2, 0, -6, -4, -2};
    const int8x16_t high_shift = {0, 2, 4, 0, 0, 2, 4, 0,
        0, 2, 4, 0, 0, 2, 4, 0};
    // Each load consumes 12 bytes. Leave the final group to the scalar
    // loop so the 16-byte load is always within the encoded payload.
    for (; i + 16 < registers_size; i += 16, packed += 12) {
        const auto bytes = vld1q_u8(packed);
        const auto low = vshlq_u8(vqtbl1q_u8(bytes, low_index), low_shift);
        const auto high = vshlq_u8(vqtbl1q_u8(bytes, high_index), high_shift);
        const auto values = vandq_u8(vorrq_u8(low, high), vdupq_n_u8(63));
        auto* destination = into.data() + i;
        vst1q_u8(destination, vmaxq_u8(vld1q_u8(destination), values));
    }
#endif
    for (; i < registers_size; i += 4, packed += 3) {
        const unsigned bits = packed[0] | (unsigned(packed[1]) << 8) |
            (unsigned(packed[2]) << 16);
        into[i] = std::max(into[i], static_cast<uint8_t>(bits & 63));
        into[i + 1] = std::max(into[i + 1], static_cast<uint8_t>((bits >> 6) & 63));
        into[i + 2] = std::max(into[i + 2], static_cast<uint8_t>((bits >> 12) & 63));
        into[i + 3] = std::max(into[i + 3], static_cast<uint8_t>(bits >> 18));
    }
}
}

bool header(const std::string& prefix, uint64_t length) {
    return prefix.size() >= header_size && length >= header_size &&
        prefix.compare(0, 4, "HYLL") == 0 &&
        (prefix[4] == 1 || (prefix[4] == 0 && length == dense_size));
}
bool cached(const std::string& value, uint64_t& result) {
    if (static_cast<uint8_t>(value[15]) & 128) return false;
    result = 0;
    for (size_t i = 0; i < 8; ++i)
        result |= static_cast<uint64_t>(static_cast<uint8_t>(value[8 + i]))
            << (8 * i);
    return true;
}
void cache(std::string& value, uint64_t result) {
    for (size_t i = 0; i < 8; ++i)
        value[8 + i] = static_cast<char>(result >> (8 * i));
}
void invalidate(std::string& value) {
    value[15] = static_cast<char>(static_cast<uint8_t>(value[15]) | 128);
}
std::string empty() {
    std::string value(header_size, '\0');
    value.replace(0, 4, "HYLL"); value[4] = 1;
    encode_run(value, registers_size, 0);
    return value;
}
uint8_t get(const std::string& value, size_t index) {
    const size_t offset = header_size + index * 6 / 8;
    const auto shift = index * 6 % 8;
    uint16_t bits = static_cast<uint8_t>(value[offset]);
    if (shift > 2) bits |= static_cast<uint16_t>(
        static_cast<uint8_t>(value[offset + 1])) << 8;
    return static_cast<uint8_t>((bits >> shift) & 63);
}
void put(std::string& value, size_t index, uint8_t frequency) {
    const size_t offset = header_size + index * 6 / 8;
    const auto shift = index * 6 % 8;
    const auto old = static_cast<uint8_t>(value[offset]);
    value[offset] = static_cast<char>((old & ~(63U << shift)) |
        (frequency << shift));
    if (shift <= 2) return;
    const auto next = static_cast<uint8_t>(value[offset + 1]);
    value[offset + 1] = static_cast<char>((next & ~(63U >> (8 - shift))) |
        (frequency >> (8 - shift)));
}
bool merge(const std::string& value, registers& into) {
    if (!header(value, value.size())) return false;
    if (!value[4]) {
        merge_dense(reinterpret_cast<const uint8_t*>(value.data() + header_size), into);
        return true;
    }
    size_t index = 0;
    for (size_t offset = header_size; offset < value.size();) {
        run part;
        if (!decode_run(value, offset, part) ||
            part.length > registers_size - index) return false;
        if (part.value) for (size_t i = 0; i < part.length; ++i)
            into[index + i] = std::max(into[index + i], part.value);
        index += part.length; offset += part.bytes;
    }
    return index == registers_size;
}

void pack_dense(std::string& value, const registers& from) {
    value.resize(dense_size);
    value[4] = 0;
    auto* packed = reinterpret_cast<uint8_t*>(&value[header_size]);
#if defined(__aarch64__) && defined(__ARM_NEON)
    const uint8x16_t low_index = {0, 1, 2, 4, 5, 6, 8, 9, 10, 12, 13, 14,
        255, 255, 255, 255};
    const uint8x16_t high_index = {1, 2, 3, 5, 6, 7, 9, 10, 11, 13, 14, 15,
        255, 255, 255, 255};
    const int8x16_t low_shift = {0, -2, -4, 0, -2, -4, 0, -2, -4, 0, -2, -4,
        0, 0, 0, 0};
    const int8x16_t high_shift = {6, 4, 2, 6, 4, 2, 6, 4, 2, 6, 4, 2,
        0, 0, 0, 0};
    for (size_t i = 0; i < registers_size; i += 16, packed += 12) {
        const auto bytes = vld1q_u8(from.data() + i);
        const auto low = vshlq_u8(vqtbl1q_u8(bytes, low_index), low_shift);
        const auto high = vshlq_u8(vqtbl1q_u8(bytes, high_index), high_shift);
        const auto result = vorrq_u8(low, high);
        // Store 12 bytes as two overlapping eight-byte writes.
        vst1_u8(packed, vget_low_u8(result));
        vst1_u8(packed + 4, vget_low_u8(vextq_u8(result, result, 4)));
    }
#else
    for (size_t i = 0; i < registers_size; i += 4, packed += 3) {
        const unsigned bits = from[i] | (unsigned(from[i + 1]) << 6) |
            (unsigned(from[i + 2]) << 12) | (unsigned(from[i + 3]) << 18);
        packed[0] = static_cast<uint8_t>(bits);
        packed[1] = static_cast<uint8_t>(bits >> 8);
        packed[2] = static_cast<uint8_t>(bits >> 16);
    }
#endif
}

bool dense(std::string& value) {
    if (!header(value, value.size())) return false;
    if (!value[4]) return true;
    registers values{};
    if (!merge(value, values)) return false;
    std::string next(value, 0, header_size);
    pack_dense(next, values);
    value.swap(next);
    return true;
}
} // namespace hll
} // namespace pkv
