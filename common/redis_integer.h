#pragma once
#include <climits>
#include <string>

namespace pkv {

// Redis command integers use canonical decimal notation, with no whitespace,
// plus sign, leading zeroes, or negative zero. Do not truncate at embedded NUL.
inline bool parse_redis_integer(const char* text, size_t size,
    long long& value) {
    if (!size) return false;
    const bool negative = text[0] == '-';
    size_t i = negative ? 1 : 0;
    // At most 19 decimal digits can fit in a signed 64-bit integer.
    // Their unsigned magnitude always fits, so check the signed range once.
    if (i == size || size - i > 19) return false;
    if (text[i] == '0' && (negative || size != 1)) return false;
    const unsigned long long limit = negative
        ? static_cast<unsigned long long>(LLONG_MAX) + 1 : LLONG_MAX;
    unsigned long long result = 0;
    for (; i < size; ++i) {
        const unsigned char digit = text[i];
        if (digit < '0' || digit > '9') return false;
        result = result * 10 + (digit - '0');
    }
    if (result > limit) return false;
    if (negative) value = result == limit ? LLONG_MIN :
        -static_cast<long long>(result);
    else value = static_cast<long long>(result);
    return true;
}

inline bool parse_redis_integer(const std::string& text, long long& value) {
    return parse_redis_integer(text.data(), text.size(), value);
}

inline bool expiration_deadline(long long amount, long long scale, long long base, long long& out) {
    if (amount > LLONG_MAX / scale || amount < LLONG_MIN / scale) return false;
    const long long delta = amount * scale;
    if ((delta > 0 && base > LLONG_MAX - delta) || (delta < 0 && base < LLONG_MIN - delta)) return false;
    out = base + delta;
    return true;
}

} // namespace pkv
