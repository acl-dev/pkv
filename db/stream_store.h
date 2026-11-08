#pragma once
#include "page_transaction.h"
#include <cstdint>

namespace pkv {

struct stream_id {
    uint64_t milliseconds = 0;
    uint64_t sequence = 0;
};

struct stream_entry {
    stream_id id;
    // Preserve field order and repeated field names.
    std::vector<std::pair<std::string, std::string>> fields;
};

struct stream_root {
    uint64_t size = 0;
    stream_id last;
    int64_t expires_at_ms = -1;
    bool compact_entries = false;
};

bool decode_stream_root(const std::string& bytes, stream_root& out);
void encode_stream_root(const stream_root& root, std::string& out);

// A dense page sequence keeps append bounded independently of stream length.
// All changes belong to the caller's page transaction.
class stream_store {
public:
    static constexpr size_t max_entry_bytes = 32 * 1024 * 1024;
    enum class status { ok, invalid, invalid_id, too_large, storage };
    stream_store(page_transaction& transaction, const std::string& key);
    status open(bool expire = false);
    // Borrow alternating field names and values for the duration of append.
    status append(stream_id id, const std::string* const* fields, size_t count);
    status read(uint64_t position, stream_entry& entry);
    status range(stream_id minimum, stream_id maximum, bool reverse,
        uint64_t limit, std::vector<stream_entry>& out);
    status clone_to(stream_store& destination);
    uint64_t size() const { return state_.size; }
    stream_id last_id() const { return state_.last; }

private:
    status read_page(uint64_t page, std::string& bytes);
    status stage_entry(uint64_t position, std::string&& bytes);
    status read_encoded(uint64_t position, std::string& bytes, stream_id& id,
        uint32_t& fields);
    status decode_header(uint64_t position, const std::string& bytes,
        stream_id& id, uint32_t& fields) const;
    status read_header(uint64_t position, std::string& bytes, stream_id& id,
        uint32_t& fields);
    page_transaction& transaction_;
    db::batch_entry* root_;
    stream_root state_;
    bool opened_ = false;
};

} // namespace pkv
