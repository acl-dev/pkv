#pragma once
#include <cstddef>
#include <cstdint>

namespace pkv {
namespace page_wire {

constexpr uint32_t magic = 0x504b5632; // PKV2
constexpr uint32_t exec_magic = 0x504b5633; // PKV3
constexpr uint32_t ordered_magic = 0x504b5634; // PKV4
constexpr uint32_t compact_magic = 0x504b5635; // PKV5
constexpr size_t header_bytes = 20;

inline uint32_t magic_for(unsigned version) {
    return version == 5 ? compact_magic : version == 4 ? ordered_magic :
        version == 3 ? exec_magic :
        version == 2 ? magic : 0;
}
inline unsigned version(uint32_t value) {
    return value == compact_magic ? 5 : value == ordered_magic ? 4 :
        value == exec_magic ? 3 :
        value == magic ? 2 : 0;
}

inline void put32(char* out, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) {
        out[i] = static_cast<char>(value >> (24 - 8 * i));
    }
}

inline void put64(char* out, uint64_t value) {
    for (unsigned i = 0; i < 8; ++i) {
        out[i] = static_cast<char>(value >> (56 - 8 * i));
    }
}

inline uint32_t get32(const char* bytes) {
    uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i) {
        value = (value << 8) | static_cast<unsigned char>(bytes[i]);
    }
    return value;
}

inline uint64_t get64(const char* bytes) {
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) {
        value = (value << 8) | static_cast<unsigned char>(bytes[i]);
    }
    return value;
}

} // namespace page_wire
} // namespace pkv
