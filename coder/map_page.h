#pragma once
#include <cstdint>
#include <cstddef>
#include <string>
#include <array>

namespace pkv {

enum class map_kind : unsigned char { hash = 1, set = 2 };
enum class map_page_status { ok, invalid, unsupported, out_of_memory };

struct map_root {
    map_kind kind = map_kind::hash;
    int64_t expires_at_ms = -1;
    uint64_t elements = 0;
    uint64_t tree = 0;
    uint64_t last_id = 0;
    bool bucketed = false;
    bool grouped = false;
};

struct map_bucket {
    uint64_t tree = 0;
    uint64_t elements = 0;
};
struct map_group {
    std::array<map_bucket, 16> buckets;
};
struct map_directory {
    std::array<map_bucket, 256> buckets;
};

struct map_blob {
    uint64_t length = 0;
    uint64_t head = 0;
    std::string inline_value;
    // A score key can keep its prefix inline and borrow the member's chain.
    bool shares_fragments() const { return head && !inline_value.empty(); }
};

// One AVL node per field/member. Small payloads stay inline; large keys and
// values use independent bounded fragment chains in the same page family.
struct map_node {
    uint64_t id = 0;
    uint8_t bucket = 0;
    bool bucket_derived = false;
    uint64_t left = 0;
    uint64_t right = 0;
    uint32_t height = 1;
    uint64_t elements = 1;
    map_blob key;
    map_blob value;
};

// Borrowed payloads are valid only while the encoded input is unchanged.
// Backend inspectors must consume this view before returning their callback.
struct map_node_view {
    struct blob {
        uint64_t length = 0, head = 0;
        const char* data = nullptr;
        bool shared = false;
        size_t inline_size() const { return head ? (shared ? 8 : 0) : size_t(length); }
    };
    uint64_t id = 0;
    uint8_t bucket = 0;
    bool bucket_derived = false;
    uint64_t left = 0, right = 0;
    uint32_t height = 1;
    uint64_t elements = 1;
    blob key, value;
};

struct map_fragment {
    uint64_t id = 0;
    uint64_t next = 0;
    uint64_t offset = 0;
    std::string bytes;
};

struct map_fragment_view {
    uint64_t id = 0, next = 0, offset = 0;
    const char* data = nullptr;
    size_t size = 0;
};

class map_page_codec {
public:
    static const size_t root_bytes = 48;
    static const size_t group_bytes = 16 + 16 * 16;
    static const size_t directory_bytes = 16 + 256 * 16;
    static const size_t node_header_bytes = 80;
    static const size_t fragment_header_bytes = 32;
    static const size_t max_page_bytes = 16384;
    static const size_t key_inline_bytes = 64;
    static const size_t zset_key_inline_bytes = 127;
    static const size_t value_inline_bytes = 128;
    static const uint64_t max_payload_bytes = 512ULL * 1024 * 1024;
    // An internal score key prefixes a legal member with eight score bytes.
    static const uint64_t max_blob_bytes = max_payload_bytes + 8;
    static const uint32_t max_height = 128;

    // Invalid records and allocation failures leave output unchanged.
    static map_page_status encode_root(const map_root&, std::string& out);
    static map_page_status decode_root(const std::string&, map_root& out);
    static map_page_status encode_directory(const map_root&,
        const map_directory&, std::string& out);
    static map_page_status decode_directory(const std::string&,
        const map_root&, map_directory& out);
    static map_page_status encode_group(const map_root&, unsigned tag,
        const map_group&, std::string& out);
    static map_page_status decode_group(const std::string&,
        const map_root&, unsigned tag, map_group& out);
    static map_page_status encode_node(const map_node&, std::string& out);
    static map_page_status encode_set_reference(const map_node&, std::string& out);
    static map_page_status encode_packed_node(const map_node&, std::string& out,
        bool hashed_bucket = false, bool compact_zset = false,
        bool compact_set_leaf = false);
    // The scratch encoder may retain up to 128 bytes of staging capacity.
    // Compact ZSet point writes can use short leaves or fragment references.
    static map_page_status encode_packed_node_scratch(const map_node&,
        std::string& out, bool hashed_bucket = false, bool compact_zset = false);
    static map_page_status decode_node(const std::string&, map_node& out);
    // Full structural validation, without allocating/copying payloads.
    // Invalid input leaves out unchanged. The view must not outlive input.
    static map_page_status decode_node_view(const std::string&,
        map_node_view& out);
    // Decode into disposable scratch. Discard the view on failure.
    static map_page_status decode_node_view_scratch(const std::string&,
        map_node_view& out);
    // Reuse owned payload capacity. Errors may change out; callers must check
    // the status before using it. Aliased input takes the strong decoder path.
    static map_page_status decode_node_scratch(const std::string&,
        map_node& out, bool with_value = true);
    static map_page_status encode_fragment(const map_fragment&,
        std::string& out);
    static map_page_status encode_fragment(uint64_t id, uint64_t next,
        uint64_t offset, const char* payload, size_t size, std::string& out,
        bool compact_single = false);
    static map_page_status decode_fragment(const std::string&,
        map_fragment& out);
    static map_page_status decode_fragment_view(const std::string&,
        map_fragment_view& out);
private:
    static map_page_status encode_packed_node_impl(const map_node&,
        std::string& out, bool hashed_bucket, bool reuse_capacity,
        bool compact_zset, bool compact_set_leaf);
};

} // namespace pkv
