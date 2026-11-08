#pragma once
#include "page_batch.h"
#include "db/family_move.h"

namespace pkv {

class exec_receiver;
class database_pool;
class exec_entry {
public:
    explicit exec_entry(std::shared_ptr<const page_batch> pages)
    : pages_(std::move(pages)) {}
    explicit exec_entry(family_move move)
    : move_(std::make_shared<const family_move>(std::move(move))) {}
    const std::shared_ptr<const page_batch>& pages() const { return pages_; }
    const std::shared_ptr<const family_move>& move() const { return move_; }
private:
    std::shared_ptr<const page_batch> pages_;
    std::shared_ptr<const family_move> move_;
};
class exec_batch {
public:
    static std::shared_ptr<const exec_batch> capture(
        std::vector<std::shared_ptr<const page_batch>> batches);
    static std::shared_ptr<const exec_batch> capture_ordered(
        std::vector<exec_entry> entries);
    const std::vector<exec_entry>& entries() const { return entries_; }
    // Minimum stream version; page-only ordered input canonicalizes to v3.
    unsigned protocol() const {
        return compact_ ? 5 : entries_.empty() ? 3 : 4;
    }
    size_t expanded_bytes() const { return expanded_bytes_; }
    size_t actions() const { return actions_; }
    bool has_moves() const;
    bool touches(unsigned database) const;
    const std::vector<std::shared_ptr<const page_batch>>& batches() const {
        return batches_;
    }
    size_t bytes() const { return bytes_; }
    size_t records() const { return records_; }
    // Cross-DB commit coordination remains separate; never apply a prefix.
    bool database(unsigned& out) const;
    db::batch_status apply(db& destination) const;
    // Mixed memory DBs prepare every native frame before publishing any.
    // Disk/workspace groups decline; single-DB replay keeps its existing path.
    db::batch_status apply(database_pool& destinations) const;
private:
    friend class exec_receiver;
    friend class ordered_receiver;
    exec_batch() = default;
    size_t bytes_ = 56, records_ = 2;
    size_t expanded_bytes_ = page_batch::group_storage_bytes, actions_ = 2;
    bool compact_ = false;
    std::vector<std::shared_ptr<const page_batch>> batches_;
    std::vector<exec_entry> entries_;
};

class exec_receiver {
public:
    using result = page_receiver::result;
    result accept(page_frame frame, std::shared_ptr<const exec_batch>& out);
    void reset();
    bool active() const { return batch_ != nullptr; }
private:
    std::shared_ptr<exec_batch> batch_;
    page_receiver pages_;
    size_t expected_batches_ = 0, expected_bytes_ = 0, expected_records_ = 0;
    size_t bytes_ = 0, records_ = 0;
    bool failed_ = false;
    bool begin(const page_frame& frame);
    bool nested_begin(const page_frame& frame) const;
    result reject();
};

} // namespace pkv
