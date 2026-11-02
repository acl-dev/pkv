#pragma once
#include "string_status.h"

namespace pkv {
namespace dao {

// A read-only snapshot under its enclosing page transaction's key locks.
// Legacy roots remain unchanged, including when another owner is overwritten.
class string_bitop_source {
public:
    string_bitop_source(shared_db& db, page_transaction& transaction,
        const std::string& key, int64_t now)
    : db_(db), transaction_(transaction), key_(key), now_(now) {}
    bool open();
    bool read(uint64_t offset, size_t size, std::string& out);
    uint64_t length() const { return length_; }
    bool exists() const { return exists_; }
    bool expired() const { return expired_; }
    dao_base::read_status error() const { return error_; }
private:
    shared_db& db_;
    page_transaction& transaction_;
    const std::string& key_;
    int64_t now_;
    uint64_t length_ = 0;
    bool exists_ = false;
    bool expired_ = false;
    dao_base::read_status error_ = dao_base::read_status::ok;
    std::unique_ptr<string_store> native_;
    std::unique_ptr<record_view> legacy_;
    bool stored();
    bool fail(dao_base::read_status error) { error_ = error; return false; }
};

} // namespace dao
} // namespace pkv
