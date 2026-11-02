#pragma once
#include "db/page_transaction.h"
#include <memory>

namespace pkv {

// Private v2 page-transaction operations. They require explicit negotiation;
// they must never be sent to a legacy or v1-only subscriber.
enum class page_operation : uint32_t {
    begin = 5,
    root_keep = 6,
    root_write = 7,
    root_erase = 8,
    page_write = 9,
    page_erase = 10,
    commit = 11,
    root_replace = 12,
    root_maintenance = 13,
    exec_begin = 14,
    exec_commit = 15,
    family_move = 16
};

struct page_frame {
    unsigned protocol = 2;
    page_operation operation = page_operation::begin;
    unsigned database = 0;
    std::string key;
    std::string value;
    uint64_t id = 0;
};

struct page_batch_size {
    size_t bytes = 0;
    size_t records = 0;
    size_t roots = 0;
    size_t pages = 0;
};

class page_receiver;

class page_batch {
public:
    static const size_t max_bytes = 64 * 1024 * 1024;
    static const size_t max_records = 65536;
    static const size_t max_key_bytes = 65525;
    static const unsigned max_databases = 65536;

    // Inputs must be sorted/deduplicated as transact_pages produces them.
    // Root KEEP declarations are included only for owners of changed pages.
    // Measurement allocates nothing; failure leaves out unchanged.
    static bool measure(unsigned database,
        const std::vector<db::batch_entry>& roots,
        const std::vector<page_change>& pages, page_batch_size& out);
    static std::shared_ptr<const page_batch> capture(unsigned database,
        const std::vector<db::batch_entry>& roots,
        const std::vector<page_change>& pages);

    unsigned database() const { return database_; }
    const page_batch_size& size() const { return size_; }
    const std::vector<db::batch_entry>& roots() const { return roots_; }
    const std::vector<page_change>& pages() const { return pages_; }
    db::batch_status apply(db& destination) const;

private:
    friend class page_receiver;
    explicit page_batch(unsigned database) : database_(database) {}
    unsigned database_;
    page_batch_size size_;
    std::vector<db::batch_entry> roots_;
    std::vector<page_change> pages_;
};

class page_receiver {
public:
    enum class result { pending, complete, error };

    // Error poisons the receiver until reset/reconnect. Complete transfers an
    // immutable owning batch; neither pending nor error can modify a database.
    result accept(page_frame frame, std::shared_ptr<const page_batch>& out);
    void reset();
    bool active() const { return batch_ != nullptr; }

private:
    std::shared_ptr<page_batch> batch_;
    page_batch_size expected_;
    size_t received_bytes_ = 0;
    bool failed_ = false;
    unsigned protocol_ = 0;

    bool begin(const page_frame& frame);
    bool append_root(page_frame& frame);
    bool append_page(page_frame& frame);
    bool finish() const;
    result reject();
};

} // namespace pkv
