#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace pkv {

// Redis Cluster uses CRC16/XMODEM and the first nonempty {...} hash tag.
inline size_t redis_hash_slot(const std::string& key) {
    size_t start = 0, length = key.size();
    const auto open = key.find('{');
    if (open != std::string::npos) {
        const auto close = key.find('}', open + 1);
        if (close != std::string::npos && close > open + 1) {
            start = open + 1;
            length = close - start;
        }
    }
    std::uint16_t crc = 0;
    for (size_t i = start; i < start + length; ++i) {
        crc ^= static_cast<std::uint16_t>(static_cast<unsigned char>(key[i])) << 8;
        for (int bit = 0; bit < 8; ++bit)
            crc = static_cast<std::uint16_t>((crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1);
    }
    return crc & 16383;
}

} // namespace pkv
