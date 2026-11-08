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

struct list_page_update {
    uint64_t id = 0, previous = 0, next = 0;
    uint32_t consumed = 0;
    bool applied = false;
    bool linked = false;
    std::string bytes;
    std::string value;
};

// Borrowed contiguous members for a synchronous list write.
class list_values_view {
public:
    list_values_view(const std::string* values, size_t count)
        : values_(values), count_(count) {}
    size_t size() const { return count_; }
    bool empty() const { return count_ == 0; }
    const std::string* begin() const { return values_; }
    const std::string* end() const { return count_ ? values_ + count_ : values_; }
    const std::string& operator[](size_t i) const { return values_[i]; }
    const std::string& back() const { return values_[count_ - 1]; }
private:
    const std::string* values_;
    size_t count_;
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
    static bool is_compact_page(const std::string& bytes) {
        return bytes.size() >= 4 && (bytes[3] == 2 || bytes[3] == 3) &&
            bytes.compare(0, 3, "PLP", 3) == 0;
    }
    // Complete members use PLP2 (32-bit length) or PLP3 (8-bit length).
    // Fragmented pages retain PLP1. Its expanded size bounds all formats.
    static list_page_status encode_packed_page(const list_page& page,
        std::string& out);
    static list_page_status encode_single_member(const list_page& page,
        const std::string& value, std::string& out);
    static list_page_status decode_page(const std::string& bytes,
        list_page& out);
    // A zero count asks the caller to use the fragmented-page reader.
    static list_page_status index_compact(const std::string& bytes,
        uint64_t index, bool reverse, uint64_t member_limit,
        uint32_t& count, list_page_update& out,
        const std::string* replacement = nullptr);
    static list_page_status range_compact(const std::string& bytes,
        uint64_t skip, uint64_t take, bool reverse, uint64_t member_limit,
        uint32_t& count, list_page_update& page,
        std::vector<std::string>& values);
    // Validate the entire page and the caller's member bound before editing.
    // Full pages return ok with applied=false; errors leave out unchanged.
    // A smaller fill target limits new packing; larger existing pages retain
    // the format maximum so old/bulk pages can still use their free space.
    static list_page_status push_end(const std::string& bytes,
        const std::vector<std::string>& values, bool left,
        uint64_t member_limit, list_page_update& out,
        size_t fill_target = max_page_bytes) {
        return push_end(bytes, {values.data(), values.size()}, left,
            member_limit, out, fill_target);
    }
    static list_page_status push_end(const std::string& bytes,
        list_values_view values, bool left,
        uint64_t member_limit, list_page_update& out,
        size_t fill_target = max_page_bytes, uint64_t neighbor = 0);
    // Pop one complete endpoint member while keeping the page nonempty.
    // Cross-page members and page deletion request the existing fallback.
    static list_page_status pop_end(const std::string& bytes, bool left,
        uint64_t member_limit, list_page_update& out);
    // A linked batch appends to values. Linked batches and opted-in endpoint
    // batches report a whole-page pop in consumed for the caller to remove.
    static list_page_status pop_compact_batch(const std::string& bytes,
        bool left, uint64_t count, uint64_t member_limit, list_page_update& out,
        std::vector<std::string>* values, uint64_t removed_neighbor = 0,
        bool allow_whole = false);
private:
    static list_page_status decode_compact(const std::string& bytes,
        list_page& out);
    static list_page_status push_compact(const std::string& bytes,
        list_values_view values, bool left,
        uint64_t member_limit, list_page_update& out, size_t fill_target,
        uint64_t neighbor);
    static list_page_status pop_compact(const std::string& bytes, bool left,
        uint64_t member_limit, list_page_update& out);
    static list_page_status pop_compact_right(const std::string& bytes,
        uint64_t member_limit, list_page_update& out);
};

} // namespace pkv
