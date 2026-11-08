#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace pkv {
namespace hll {
static const size_t registers_size = 16384;
static const size_t header_size = 16;
static const size_t dense_size = 12304;
static const size_t sparse_limit = 3000;
static const size_t maximum_size = header_size + 2 * registers_size;
using registers = std::array<uint8_t, registers_size>;
bool header(const std::string& prefix, uint64_t length);
bool cached(const std::string& value, uint64_t& count);
void cache(std::string& value, uint64_t count);
void invalidate(std::string& value);
std::string empty();
uint64_t hash(const std::string& value);
uint8_t get(const std::string& value, size_t index);
void put(std::string& value, size_t index, uint8_t count);
bool merge(const std::string& value, registers& into);
bool dense(std::string& value);
bool set(std::string& value, size_t index, uint8_t count, bool& changed);
bool add(std::string& value, const std::string& element, bool& changed);
uint64_t count(const registers& values);
bool count_encoded(const std::string& value, uint64_t& result);
struct run {
    size_t length, bytes;
    uint8_t value;
    run(size_t length = 0, size_t bytes = 0, uint8_t value = 0)
        : length(length), bytes(bytes), value(value) {}
};
inline bool decode_run(const std::string& value, size_t offset, run& out) {
    if (offset >= value.size()) return false;
    const auto byte = static_cast<uint8_t>(value[offset]);
    if (byte & 128) {
        out = {size_t(byte & 3) + 1, 1,
            static_cast<uint8_t>(((byte >> 2) & 31) + 1)};
    } else if (byte & 64) {
        if (offset + 1 >= value.size()) return false;
        out = {(size_t(byte & 63) << 8) +
            static_cast<uint8_t>(value[offset + 1]) + 1, 2, 0};
    } else {
        out = {size_t(byte & 63) + 1, 1, 0};
    }
    return true;
}
inline void encode_run(std::string& value, size_t length, uint8_t frequency) {
    if (!length) return;
    if (frequency) value += static_cast<char>(128 |
        ((frequency - 1) << 2) | (length - 1));
    else if (length <= 64) value += static_cast<char>(length - 1);
    else {
        value += static_cast<char>(64 | ((length - 1) >> 8));
        value += static_cast<char>((length - 1) & 255);
    }
}
} // namespace hll
} // namespace pkv
