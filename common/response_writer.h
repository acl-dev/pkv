#pragma once
#include <algorithm>
#include <climits>
#include <cstddef>

namespace pkv {
// ACL reports individual writes as int. Keep every call within that range.
template<class Sender>
bool response_write(size_t length, size_t& written, Sender sender) {
    const size_t limit = std::min<size_t>(INT_MAX, 16ULL * 1024 * 1024);
    written = 0;
    while (written < length) {
        const auto count = std::min(limit, length - written);
        const auto delivered = sender(written, count);
        if (delivered > count) return false;
        written += delivered;
        if (delivered != count) return false;
    }
    return true;
}
} // namespace pkv
