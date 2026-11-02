#pragma once
#include <array>
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

// Stable buckets retain members' locations while surrounding members change.
// These cursors are implementation-specific and independent of cluster slots.
inline size_t scan_bucket(const std::string& value) {
    unsigned long long hash = 14695981039346656037ULL;
    for (unsigned char ch : value) { hash ^= ch; hash *= 1099511628211ULL; }
    return static_cast<size_t>(hash & 255);
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
