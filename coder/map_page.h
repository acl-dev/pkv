#pragma once
#include <cstdint>
#include <cstddef>
#include <string>

namespace pkv {

enum class map_kind : unsigned char { hash = 1, set = 2 };
enum class map_page_status { ok, invalid, unsupported, out_of_memory };

struct map_root {
    map_kind kind = map_kind::hash;
    int64_t expires_at_ms = -1;
    uint64_t elements = 0;
    uint64_t tree = 0;
    uint64_t last_id = 0;
};

struct map_blob {
    uint64_t length = 0;
    uint64_t head = 0;
    std::string inline_value;
};

// One AVL node per field/member. Small payloads stay inline; large keys and
// values use independent bounded fragment chains in the same page family.
struct map_node {
    uint64_t id = 0;
    uint8_t bucket = 0;
    uint64_t left = 0;
    uint64_t right = 0;
    uint32_t height = 1;
    uint64_t elements = 1;
    map_blob key;
    map_blob value;
};

struct map_fragment {
    uint64_t id = 0;
    uint64_t next = 0;
    uint64_t offset = 0;
    std::string bytes;
};

class map_page_codec {
public:
    static const size_t root_bytes = 48;
    static const size_t node_header_bytes = 80;
    static const size_t fragment_header_bytes = 32;
    static const size_t max_page_bytes = 16384;
    static const size_t key_inline_bytes = 64;
    static const size_t value_inline_bytes = 128;
    static const uint64_t max_payload_bytes = 512ULL * 1024 * 1024;
    // An internal score key prefixes a legal member with eight score bytes.
    static const uint64_t max_blob_bytes = max_payload_bytes + 8;
    static const uint32_t max_height = 128;

    // Invalid records and allocation failures leave output unchanged.
    static map_page_status encode_root(const map_root&, std::string& out);
    static map_page_status decode_root(const std::string&, map_root& out);
    static map_page_status encode_node(const map_node&, std::string& out);
    static map_page_status decode_node(const std::string&, map_node& out);
    static map_page_status encode_fragment(const map_fragment&,
        std::string& out);
    static map_page_status decode_fragment(const std::string&,
        map_fragment& out);
};

} // namespace pkv
