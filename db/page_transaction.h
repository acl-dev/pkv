#pragma once
#include "db.h"
#include <map>
#include <utility>

namespace pkv {

struct page_change {
    std::string owner;
    uint64_t id;
    std::string value;
    db::update_action action;

    page_change(std::string key, uint64_t number, std::string bytes,
        db::update_action operation)
    : owner(std::move(key))
    , id(number)
    , value(std::move(bytes))
    , action(operation)
    {}
};

// Borrowed immutable input to ordered replay. Owners must retain these vectors
// until replica_apply_pages returns; none may be concurrently modified.
struct page_update {
    const std::vector<db::batch_entry>& roots;
    const std::vector<page_change>& pages;
    // Local workspace snapshots; replay defaults to receiver-side waiters.
    unsigned ready_types = ~0U;
    page_update(const std::vector<db::batch_entry>& metadata,
        const std::vector<page_change>& changes) : roots(metadata),
        pages(changes) {}
};

// A page address is (logical key, nonzero page id) in a separate native
// namespace. No prefix in the user-key byte space is reserved. This view and
// its roots cannot outlive the callback or be used from another thread.
class page_transaction {
public:
    static const size_t max_value_bytes = 16384;
    db::batch_view roots() const { return roots_; }

    // Reads observe staged writes/deletes. Missing/error leave out unchanged.
    // A rejected address/write poisons the transaction, even if ignored by
    // the callback. Inputs are copied; returned data owns its bytes.
    db::lookup_status lookup(const std::string& owner, uint64_t id,
        std::string& out);
    bool put(const std::string& owner, uint64_t id, const std::string& bytes);
    bool erase(const std::string& owner, uint64_t id);
    // Discard prior staged pages and clear the native family at commit.
    // Caller must explicitly stage a root write after selecting new metadata.
    bool reset_pages(const std::string& owner);
    // Seal a complete replacement: require staged writes at IDs 1..last,
    // discard staged changes beyond last and drop the old family at commit.
    // Keeps old pages readable until the producer has staged every new page.
    bool replace_pages(const std::string& owner, uint64_t last);
    void abort() { failed_ = true; }

private:
    friend class db;
    page_transaction(db& database, db::batch_view roots)
    : database_(database), roots_(roots) {}

    using address = std::pair<std::string, uint64_t>;
    db& database_;
    db::batch_view roots_;
    std::map<address, page_change> changes_;
    bool failed_ = false;
    bool storage_error_ = false;

    bool valid_address(const std::string& owner, uint64_t id);
    bool stage(const std::string& owner, uint64_t id,
        const std::string& bytes, db::update_action action);
    bool valid_changes() const;
    std::vector<page_change> release_changes();
};

} // namespace pkv
