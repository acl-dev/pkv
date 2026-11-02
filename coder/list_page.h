#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace pkv {

// Storage-format primitives. Addresses belong in a separate backend family;
// arbitrary user keys must never share a reserved-prefix namespace with pages.
// This codec does not allocate ids, publish roots or perform transactions.
enum class list_page_status {
    ok, invalid, unsupported_version, out_of_memory
};

struct list_root {
    int64_t expires_at_ms = -1;
    uint64_t elements = 0;
    uint64_t head = 0;
    uint64_t tail = 0;
    uint64_t last_id = 0; // Zero denotes the legacy root without a counter.
};

struct list_fragment {
    uint64_t total_bytes = 0;
    uint64_t offset = 0;
    std::string bytes;
};

struct list_page {
    uint64_t id = 0;
    uint64_t previous = 0;
    uint64_t next = 0;
    std::vector<list_fragment> fragments;
};

class list_page_codec {
public:
    static const size_t root_bytes = 40;
    static const size_t extended_root_bytes = 48;
    static const size_t page_header_bytes = 32;
    static const size_t fragment_header_bytes = 24;
    static const size_t max_page_bytes = 16384;
    static const size_t max_fragment_bytes =
        max_page_bytes - page_header_bytes - fragment_header_bytes;

    // Invalid input and allocation failures leave the output unchanged.
    static list_page_status encode_root(const list_root& root,
        std::string& out);
    static list_page_status decode_root(const std::string& bytes,
        list_root& out);
    static list_page_status encode_page(const list_page& page,
        std::string& out);
    static list_page_status decode_page(const std::string& bytes,
        list_page& out);
};

} // namespace pkv
