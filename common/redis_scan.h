#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>
#include "redis_glob.h"

namespace pkv {
struct scan_options {
    unsigned long long cursor = 0;
    size_t count = 10;
    bool has_match = false;
    std::string pattern;
    bool has_type = false;
    std::string type;
    bool no_values = false;
};
struct scan_page {
    unsigned long long cursor = 0;
    std::vector<std::string> values;
};

namespace detail {
constexpr uint32_t scan_bucket_seed = 0x25;
inline uint32_t scan_bucket_bytes(const char* data, size_t size, uint32_t hash) {
    // Only the low byte is observable. The low bytes of the FNV offset
    // and prime reproduce that byte without 64-bit arithmetic.
    constexpr uint32_t prime = 0xb3;
    while (size >= 8) {
        hash = (hash ^ static_cast<unsigned char>(data[0])) * prime;
        hash = (hash ^ static_cast<unsigned char>(data[1])) * prime;
        hash = (hash ^ static_cast<unsigned char>(data[2])) * prime;
        hash = (hash ^ static_cast<unsigned char>(data[3])) * prime;
        hash = (hash ^ static_cast<unsigned char>(data[4])) * prime;
        hash = (hash ^ static_cast<unsigned char>(data[5])) * prime;
        hash = (hash ^ static_cast<unsigned char>(data[6])) * prime;
        hash = (hash ^ static_cast<unsigned char>(data[7])) * prime;
        data += 8;
        size -= 8;
    }
    if (size >= 4) {
        hash = (hash ^ static_cast<unsigned char>(data[0])) * prime;
        hash = (hash ^ static_cast<unsigned char>(data[1])) * prime;
        hash = (hash ^ static_cast<unsigned char>(data[2])) * prime;
        hash = (hash ^ static_cast<unsigned char>(data[3])) * prime;
        data += 4;
        size -= 4;
    }
    for (size_t i = 0; i < size; ++i)
        hash = (hash ^ static_cast<unsigned char>(data[i])) * prime;
    return hash;
}
uint32_t scan_bucket_wide(const char* data, size_t size, uint32_t hash);
inline uint32_t scan_bucket_update(const char* data, size_t size, uint32_t hash) {
    return size >= 1024 ? scan_bucket_wide(data, size, hash) :
        scan_bucket_bytes(data, size, hash);
}
}

// Stable buckets retain members' locations while surrounding members change.
// These cursors are implementation-specific and independent of cluster slots.
inline size_t scan_bucket(const char* data, size_t size) {
    return detail::scan_bucket_update(data, size, detail::scan_bucket_seed) & 255;
}
inline size_t scan_bucket(const std::string& value) {
    return scan_bucket(value.data(), value.size());
}
inline size_t scan_window(const std::array<size_t, 256>& sizes, const scan_options& options, scan_page& out) {
    out = scan_page();
    if (options.cursor >= sizes.size()) return sizes.size();
    size_t end = static_cast<size_t>(options.cursor), visited = 0;
    do {
        // Only compare/subtract the hint; huge COUNT values cannot wrap sums.
        if (sizes[end] >= options.count - visited) { ++end; break; }
        visited += sizes[end++];
    } while (end < sizes.size());
    out.cursor = end < sizes.size() ? end : 0;
    return end;
}
inline bool scan_accept(const std::string& name, const scan_options& options) {
    return !options.has_match || redis_glob_match(options.pattern, name);
}
template<class Range, class Name, class Emit>
void scan_collection(const Range& range, Name name, Emit emit, const scan_options& options, scan_page& out) {
    std::array<size_t, 256> sizes{};
    for (const auto& entry : range) ++sizes[scan_bucket(name(entry))];
    const auto end = scan_window(sizes, options, out);
    for (const auto& entry : range) {
        const auto& key = name(entry);
        const auto bucket = scan_bucket(key);
        if (bucket >= options.cursor && bucket < end && scan_accept(key, options)) emit(entry, out.values);
    }
}
bool parse_scan_cursor(const std::string& text, unsigned long long& out);
}
