#include "redis_scan.h"
#include "redis_integer.h"
#include <cerrno>
#include <cstdlib>
#include <cstring>

namespace pkv {

uint32_t detail::scan_bucket_wide(const char* data, size_t size, uint32_t hash) {
    // An FNV step permutes the low bits modulo every power of two.
    // A fixed block does the same. Squaring a permutation that fixes the
    // low k bits also fixes bit k, so 256 repeats fix the entire low byte.
    constexpr size_t period = 256;
    constexpr size_t block = 8 * period;
    while (size >= period) {
        if (size >= block && std::memcmp(data, data + 8, block - 8) == 0) {
            data += block;
            size -= block;
            continue;
        }
        if (std::memcmp(data, data + 1, period - 1) != 0)
            hash = scan_bucket_bytes(data, period, hash);
        data += period;
        size -= period;
    }
    return scan_bucket_bytes(data, size, hash);
}

bool parse_scan_cursor(const std::string& text, unsigned long long& out) {
    long long signed_value;
    if (parse_redis_integer(text, signed_value)) {
        if (signed_value < 0) return false;
        out = static_cast<unsigned long long>(signed_value);
        return true;
    }
    if (text.empty()) return false;
    char* end = nullptr;
    errno = 0;
    const auto value = std::strtoull(text.c_str(), &end, 10);
    if (errno == ERANGE || end == text.c_str() || end != text.c_str() + text.size()) return false;
    out = value;
    return true;
}
}
