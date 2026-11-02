#include "acl_policy.h"
#include <array>
#include <cstdint>
#include <cstring>

namespace pkv {
namespace {
// SHA-256 compression and padding, FIPS 180-4 sections 5 and 6.2.
uint32_t rotate(uint32_t value, unsigned bits) {
    return (value >> bits) | (value << (32 - bits));
}
void compress(std::array<uint32_t, 8>& state, const unsigned char* block) {
    static const uint32_t constants[] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
        0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
        0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
        0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
        0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
        0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
        0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
        0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
        0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
    };
    uint32_t words[64];
    for (size_t i = 0; i < 16; ++i) {
        words[i] = 0;
        for (size_t j = 0; j < 4; ++j)
            words[i] = (words[i] << 8) | block[4 * i + j];
    }
    for (size_t i = 16; i < 64; ++i) {
        const auto a = words[i - 15], b = words[i - 2];
        const auto small0 = rotate(a, 7) ^ rotate(a, 18) ^ (a >> 3);
        const auto small1 = rotate(b, 17) ^ rotate(b, 19) ^ (b >> 10);
        words[i] = words[i - 16] + small0 + words[i - 7] + small1;
    }
    auto work = state;
    for (size_t i = 0; i < 64; ++i) {
        const auto a = work[0], e = work[4];
        const auto big1 = rotate(e, 6) ^ rotate(e, 11) ^ rotate(e, 25);
        const auto choose = (e & work[5]) ^ (~e & work[6]);
        const auto t1 = work[7] + big1 + choose + constants[i] + words[i];
        const auto big0 = rotate(a, 2) ^ rotate(a, 13) ^ rotate(a, 22);
        const auto majority = (a & work[1]) ^ (a & work[2]) ^
            (work[1] & work[2]);
        for (size_t j = 7; j > 0; --j) work[j] = work[j - 1];
        work[4] += t1; work[0] = t1 + big0 + majority;
    }
    for (size_t i = 0; i < 8; ++i) state[i] += work[i];
}
} // namespace
std::string acl_password_hash(const std::string& password) {
    std::array<uint32_t, 8> state{{0x6a09e667, 0xbb67ae85,
        0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c,
        0x1f83d9ab, 0x5be0cd19}};
    const auto* bytes = reinterpret_cast<const unsigned char*>(password.data());
    size_t offset = 0;
    while (password.size() - offset >= 64) {
        compress(state, bytes + offset); offset += 64;
    }
    unsigned char tail[128] = {};
    const auto remaining = password.size() - offset;
    std::memcpy(tail, bytes + offset, remaining);
    tail[remaining] = 0x80;
    const size_t length = remaining < 56 ? 64 : 128;
    const auto bits = static_cast<uint64_t>(password.size()) * 8;
    for (size_t i = 0; i < 8; ++i)
        tail[length - 1 - i] = static_cast<unsigned char>(bits >> (8 * i));
    compress(state, tail);
    if (length == 128) compress(state, tail + 64);
    static const char hex[] = "0123456789abcdef";
    std::string result(64, '0');
    for (size_t i = 0; i < 64; ++i)
        result[i] = hex[(state[i / 8] >> (28 - (i % 8) * 4)) & 15];
    return result;
}
bool acl_secret_equal(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    volatile unsigned char different = 0;
    for (size_t i = 0; i < a.size(); ++i)
        different = static_cast<unsigned char>(different |
            (static_cast<unsigned char>(a[i]) ^
             static_cast<unsigned char>(b[i])));
    return different == 0;
}
} // namespace pkv
