#pragma once
#include <cstddef>
#include <climits>
#include <vector>

namespace pkv {

// List ranges differ from GETRANGE: an end before the first item stays empty.
inline bool list_slice(size_t length, long long start, long long end, size_t& offset, size_t& count) {
    offset = count = 0;
    if (!length) return false;
    const auto size = static_cast<long long>(length);
    if (start < 0) start += size;
    if (end < 0) end += size;
    if (start < 0) start = 0;
    if (start > end || start >= size) return false;
    if (end >= size) end = size - 1;
    offset = static_cast<size_t>(start);
    count = static_cast<size_t>(end - start + 1);
    return true;
}

inline bool list_index(size_t length, long long index, size_t& offset) {
    if (index < 0) index += static_cast<long long>(length);
    if (index < 0 || static_cast<unsigned long long>(index) >= length) return false;
    offset = static_cast<size_t>(index);
    return true;
}

struct list_search {
    long long rank = 1;
    long long count = 1; // zero means all matches
    long long maxlen = 0; // zero means the entire list
};

// Caller-provided comparison lets a codec scan decoded storage without copying
// string elements. Indexes always refer to the original head of the list.
template<class Matches>
bool find_list_positions(size_t size, const list_search& options, Matches matches, std::vector<long long>& out) {
    out.clear();
    if (options.rank == 0 || options.rank == LLONG_MIN || options.count < 0 || options.maxlen < 0) return false;
    const bool reverse = options.rank < 0;
    auto skip = static_cast<unsigned long long>(reverse ? -(options.rank + 1) : options.rank - 1);
    const auto limit = options.maxlen && static_cast<unsigned long long>(options.maxlen) < size
        ? static_cast<size_t>(options.maxlen) : size;
    for (size_t scanned = 0; scanned < limit; ++scanned) {
        const size_t index = reverse ? size - scanned - 1 : scanned;
        if (!matches(index)) continue;
        if (skip) { --skip; continue; }
        out.push_back(static_cast<long long>(index));
        if (options.count && out.size() >= static_cast<unsigned long long>(options.count)) break;
    }
    return true;
}

} // namespace pkv
