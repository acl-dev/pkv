#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace pkv {

enum class string_page_status { ok, invalid, unsupported, out_of_memory };

struct string_root_header {
    int64_t expires_at_ms = -1;
    uint64_t length = 0;
    uint64_t pages = 0;
    size_t payload_offset = 0;
};

struct string_root {
    int64_t expires_at_ms = -1;
    uint64_t length = 0;
    uint64_t pages = 0;
    std::string inline_value;
};

// PSR roots are bounded. Large values use deterministic private page IDs
// 1..pages; all but the last data page contain exactly page_bytes raw bytes.
class string_page_codec {
public:
    static const size_t header_bytes = 32;
    static const size_t inline_bytes = 1024;
    static const size_t page_bytes = 16384;
    static const uint64_t write_bytes = 512ULL * 1024 * 1024;
    // BITFIELD validates its starting bit offset; a 64-bit field can extend
    // up to eight bytes past the ordinary String write boundary.
    static const uint64_t max_bytes = write_bytes + 8;
    static uint64_t pages_for(uint64_t length);
    static size_t page_size(const string_root& root, uint64_t index);
    static string_page_status encode_root(const string_root& root,
        std::string& out);
    static string_page_status encode_inline(int64_t expires_at_ms,
        const std::string& value, std::string& out, bool compact = false);
    static bool compact_inline(const std::string& bytes);
    // Canonical decimal String root, without an intermediate owning payload.
    static string_page_status encode_integer(int64_t expires_at_ms,
        long long value, std::string& out);
    static string_page_status decode_header(const std::string& bytes,
        string_root_header& out);
    static string_page_status decode_root(const std::string& bytes,
        string_root& out);
};

} // namespace pkv
