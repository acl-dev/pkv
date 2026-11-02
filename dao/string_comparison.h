#pragma once
#include "coder/record_codec.h"
#include "coder/string_page.h"
#include "common/string_digest.h"
#include <algorithm>

namespace pkv {
namespace dao {
// Only the requested chunk is owned. A mismatch needs no further page reads.
template<class Reader>
bool string_equal_chunks(uint64_t length, const std::string& expected,
    const Reader& read, bool& equal) {
    equal = false;
    if (length != expected.size()) return true;
    std::string chunk;
    for (uint64_t offset = 0; offset < length;) {
        const auto count = static_cast<size_t>(std::min<uint64_t>(
            string_page_codec::page_bytes, length - offset));
        if (!read(static_cast<long long>(offset),
            static_cast<long long>(offset + count - 1), chunk) ||
            chunk.size() != count) return false;
        if (expected.compare(static_cast<size_t>(offset), count, chunk))
            return true;
        offset += count;
    }
    equal = true;
    return true;
}
template<class Reader>
bool string_digest_chunks(uint64_t length, const Reader& read,
    std::string& out) {
    out.clear();
    string_digest digest;
    std::string chunk;
    for (uint64_t offset = 0; offset < length;) {
        const auto count = static_cast<size_t>(std::min<uint64_t>(
            string_page_codec::page_bytes, length - offset));
        if (!read(static_cast<long long>(offset),
            static_cast<long long>(offset + count - 1), chunk) ||
            chunk.size() != count) return false;
        digest.update(chunk); offset += count;
    }
    out = digest.finish(); return true;
}
inline bool record_string_digest(const record_view& view, std::string& out) {
    size_t length;
    return view.string_length(length) && string_digest_chunks(length,
        [&](long long first, long long last, std::string& bytes) {
            return view.string_range(first, last, bytes);
        }, out);
}
inline bool record_string_equal(const record_view& view,
    const std::string& expected, bool& equal) {
    size_t length;
    return view.string_length(length) && string_equal_chunks(length, expected,
        [&](long long first, long long last, std::string& out) {
            return view.string_range(first, last, out);
        }, equal);
}
} // namespace dao
} // namespace pkv
