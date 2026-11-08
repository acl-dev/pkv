#pragma once
#include "db.h"
#include "rpc/page_batch.h"
#include <map>

namespace pkv {

// One actor owns exclusive admission until destruction. Commands use ordinary
// DB/page APIs against this view; only commit() changes the source database.
class page_workspace : public db {
public:
    explicit page_workspace(shared_db source,
        size_t byte_limit = page_batch::max_bytes,
        size_t record_limit = page_batch::max_records);
    ~page_workspace() override;
    batch_status commit();
    void discard();
    size_t pending_bytes() const { return batches_.empty() ? 0 : bytes_; }
    size_t pending_records() const { return batches_.empty() ? 0 : records_; }
    size_t pending_batches() const { return batches_.size(); }
    const char* get_dbtype() const override { return source_->get_dbtype(); }
protected:
    bool dbopen(const char*) override { return false; }
    bool dbset(const std::string& key, const std::string& value) override;
    bool dbget(const std::string& key, std::string& out) override;
    lookup_status dblookup(const std::string& key, std::string& out) override;
    bool dbdel(const std::string& key) override;
    bool dbsupports_pages() const override { return true; }
    lookup_status dblookup_page(const std::string& owner, uint64_t id,
        std::string& out) override;
    batch_status dbapply(const std::vector<batch_entry>& roots) override;
    batch_status dbapply_pages(const std::vector<batch_entry>& roots,
        const std::vector<page_change>& pages) override;
    count_status dbkey_count(uint64_t& out) override;
    scan_status dbvisit_keys(const key_visitor& visit) override;
    scan_status dbscan_keys(unsigned long long cursor, size_t count,
        std::vector<std::string>& keys, unsigned long long& next) override;
    db_cursor* dbcreate_cursor() override { return nullptr; }
    bool dbscan(size_t, db_cursor&, std::vector<std::string>&,
        size_t) override { return false; }
    size_t dbsize() const override;
private:
    friend class replica_group;
    struct page_state {
        const page_change* page = nullptr;
        uint64_t generation = 0;
    };
    using page_index = std::map<uint64_t, page_state>;
    struct owner_state {
        const batch_entry* root = nullptr;
        uint64_t generation = 0;
        bool reset = false;
        bool initial_found = false, present = false;
        bool logical = false, history = false;
        page_index pages;
    };
    using position = std::pair<owner_state*, page_index::iterator>;
    execution_guard admission_;
    shared_db source_;
    const size_t byte_limit_, record_limit_;
    size_t bytes_ = 56, records_ = 2;
    size_t expanded_bytes_ = page_batch::group_storage_bytes, actions_ = 2;
    bool finished_ = false;
    int64_t root_count_delta_ = 0;
    std::map<std::string, owner_state> owners_;
    struct staged_batch {
        std::shared_ptr<const page_batch> batch;
        unsigned ready_types;
    };
    std::vector<staged_batch> batches_;
    void prepare_commit(std::vector<batch_entry>& roots,
        std::vector<page_change>& pages) const;
    batch_status stage(std::shared_ptr<const page_batch> batch);
    void publish_indexes(const page_batch& batch,
        const std::vector<position>& positions);
};
} // namespace pkv
