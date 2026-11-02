#pragma once
#include "page_transaction.h"
#include "coder/string_page.h"
#include "common/redis_bitmap.h"

namespace pkv {

// A typed String context belongs to one existing key transaction. Empty String
// values exist independently of their byte length; clear() removes the key.
class string_store {
public:
    enum class status {
        ok, not_paged, invalid, unsupported, storage, too_large,
        invalid_argument
    };
    string_store(page_transaction& transaction, const std::string& owner);
    ~string_store();
    string_store(const string_store&) = delete;
    string_store& operator=(const string_store&) = delete;
    bool open();
    void initialize(int64_t expires_at_ms = -1);
    bool exists() const { return exists_; }
    const string_root& metadata() const { return root_; }
    status error() const { return error_; }
    bool assign(const std::string& value);
    using chunk_source = std::function<bool(uint64_t, size_t, std::string&)>;
    // Produce chunks in increasing page order. The producer may read old
    // inputs at the current offset, including this owner, before its page is
    // replaced. It must not depend on an already produced old page. Clears
    // TTL and old pages atomically; a failed producer poisons the transaction.
    bool replace_chunks(uint64_t length, const chunk_source& source);
    bool append(const std::string& value);
    bool write(uint64_t offset, const std::string& value);
    bool write_field(uint64_t offset, const std::string& value);
    bool read(std::string& out);
    bool range(long long start, long long end, std::string& out);
    bool getbit(uint64_t offset, long long& bit);
    bool setbit(uint64_t offset, bool bit, long long& previous);
    bool bitcount(const bitmap_range& range, long long& count);
    bool bitpos(bool bit, const bitmap_range& range, long long& position);
    bool clone_to(string_store& destination, bool& copied);
    bool set_expiry(int64_t expires_at_ms);
    void touch() { if (exists_) dirty_ = true; }
    bool clear();
    bool finish();
private:
    page_transaction& transaction_;
    const std::string owner_;
    db::batch_entry* entry_;
    string_root root_;
    status error_ = status::ok;
    bool exists_ = false;
    bool dirty_ = false;
    bool fail(status value);
    bool decoded(string_page_status value);
    bool read_page(uint64_t index, std::string& out);
    bool write_page(uint64_t index, const std::string& value);
    bool scan_bits(uint64_t first, uint64_t last, int bit, long long& out);
    bool write_limit(uint64_t offset, const std::string& value, uint64_t limit);
};

} // namespace pkv
