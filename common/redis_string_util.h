#pragma once
#include <cstddef>
#include <climits>

namespace pkv {

static const size_t redis_string_max = 512ULL * 1024 * 1024;

inline bool checked_integer_add(long long value, long long delta, long long& out) {
    if ((delta > 0 && value > LLONG_MAX - delta) ||
        (delta < 0 && value < LLONG_MIN - delta)) return false;
    out = value + delta;
    return true;
}

// GETRANGE has inclusive bounds and clamps negative indexes after adding size.
inline bool string_slice(size_t length, long long start, long long end, size_t& offset, size_t& count) {
    offset = count = 0;
    if (length == 0 || (start < 0 && end < 0 && start > end)) return false;
    const auto size = static_cast<long long>(length);
    if (start < 0) start += size;
    if (end < 0) end += size;
    if (start < 0) start = 0;
    if (end < 0) end = 0;
    if (end >= size) end = size - 1;
    if (start > end) return false;
    offset = static_cast<size_t>(start);
    count = static_cast<size_t>(end - start + 1);
    return true;
}

} // namespace pkv
