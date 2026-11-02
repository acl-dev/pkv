#include "redis_hll.h"
#include <algorithm>

namespace pkv {
namespace hll {
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
bool decode_run(const std::string& value, size_t offset, run& out) {
    if (offset >= value.size()) return false;
    const auto byte = static_cast<uint8_t>(value[offset]);
    out = run(); out.bytes = 1;
    if (byte & 128) {
        out.length = (byte & 3) + 1; out.value = ((byte >> 2) & 31) + 1;
    } else if (byte & 64) {
        if (offset + 1 >= value.size()) return false;
        out.bytes = 2;
        out.length = ((byte & 63) << 8) +
            static_cast<uint8_t>(value[offset + 1]) + 1;
    } else out.length = (byte & 63) + 1;
    return true;
}
void encode_run(std::string& value, size_t length, uint8_t frequency) {
    if (!length) return;
    if (frequency) value += static_cast<char>(128 |
        ((frequency - 1) << 2) | (length - 1));
    else if (length <= 64) value += static_cast<char>(length - 1);
    else {
        value += static_cast<char>(64 | ((length - 1) >> 8));
        value += static_cast<char>((length - 1) & 255);
    }
}
bool merge(const std::string& value, registers& into) {
    if (!header(value, value.size())) return false;
    if (!value[4]) {
        for (size_t i = 0; i < registers_size; ++i)
            into[i] = std::max(into[i], get(value, i));
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
bool dense(std::string& value) {
    if (!header(value, value.size())) return false;
    if (!value[4]) return true;
    registers values{};
    if (!merge(value, values)) return false;
    std::string next(value, 0, header_size);
    next.resize(dense_size, '\0'); next[4] = 0;
    for (size_t i = 0; i < registers_size; ++i) put(next, i, values[i]);
    value.swap(next); return true;
}
} // namespace hll
} // namespace pkv
