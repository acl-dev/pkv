#pragma once
#if defined(HAS_TBB)
#include "../db.h"
#include <tbb/concurrent_hash_map.h>
#include <acl-lib/fiber/fiber_mutex.hpp>
#include <array>
#include <atomic>
#include "page_index.h"
#include "common/dense_key_index.h"

namespace pkv {

class mdb_tbb : public db {
public:
    explicit mdb_tbb(size_t count);
    ~mdb_tbb() override;
protected:
    bool dbopen(const char*) override;
    bool dbset(const std::string& key, const std::string& value) override;
    bool dbget(const std::string& key, std::string& value) override;
    bool dbinline_strings() const override { return true; }
    inline_status dbread_inline(const std::string& key,
        std::string& out, size_t offset, size_t limit) override;
    bool dbdel(const std::string& key) override;
    lookup_status dblookup(const std::string& key, std::string& value) override;
    db_cursor* dbcreate_cursor() override { return nullptr; }
    bool dbscan(size_t, db_cursor&, std::vector<std::string>&, size_t) override;
    count_status dbkey_count(uint64_t& out) override;
    scan_status dbkey_at_rank(uint64_t rank, std::string& out,
        bool& found) override;
    scan_status dbvisit_keys(const key_visitor& visit) override;
    scan_status dbscan_keys(unsigned long long cursor, size_t count,
        std::vector<std::string>& keys, unsigned long long& next) override;
    batch_status dbapply(const std::vector<batch_entry>& entries) override;
    bool dbsupports_pages() const override { return true; }
    bool dbsupports_prepare() const override { return true; }
    bool dbsupports_owned_prepare() const override { return true; }
    batch_status dbprepare_owned_pages(const std::vector<batch_entry>& roots,
        const std::vector<page_change>& pages,
        std::unique_ptr<prepared_batch>& out) override;
    bool dbsupports_family_transfer(const db& other) const override;
    bool dbsupports_detached_pages() const override { return true; }
    batch_status dbdetach_pages(const std::string& owner,
        std::unique_ptr<prepared_pages>& out) override;
    lookup_status dblookup_bounded_root(const std::string& owner,
        std::string& out) override;
    bool dbsupports_page_stream() const override { return true; }
    batch_status dbvisit_pages(const std::string& owner,
        const page_visitor& visit) override;
    batch_status dbprepare_pages(const std::vector<batch_entry>& roots,
        const std::vector<page_change>& pages,
        prepare_callback& ready) override;
    lookup_status dblookup_page(const std::string& owner, uint64_t id,
        std::string& value) override;
    bool dbbind_page_reader(const std::string& owner, page_reader& reader) override;
    batch_status dbapply_pages(const std::vector<batch_entry>& roots,
        const std::vector<page_change>& pages) override;
    size_t dbsize() const override { return stores_.size(); }
    const char* get_dbtype() const override { return "mdb_tbb"; }
private:
    friend class db;
    static const size_t scan_buckets = 256;
    static constexpr size_t scan_lock_count = 8;
    using page_map = mdb_page_index_impl<false>;
    struct record { std::string root; page_map pages; };
    using record_ptr = std::shared_ptr<record>;
    batch_status replace_bound_page(const batch_entry& root,
        const page_change& page, const page_reader& reader);
    static batch_status replace_existing_page(const batch_entry& root,
        const page_change& page, record* owner);
    struct owner_entry {
        record_ptr data;
        mutable size_t position = 0;
        owner_entry() = default;
        explicit owner_entry(record_ptr value) : data(std::move(value)) {}
    };
    using owner_value = std::pair<const std::string, owner_entry>;
    using owner_map = tbb::concurrent_hash_map<std::string, owner_entry,
        tbb::tbb_hash_compare<std::string>, std::allocator<owner_value>>;
    // Key addresses stay stable in oneTBB nodes. The index holds no duplicate
    // key strings; a shard lock stripe protects them during SCAN copying.
    struct scan_index {
        dense_key_index<owner_value> keys;
    };
    struct store {
        owner_map owners;
        std::array<std::atomic<scan_index*>, scan_buckets> indexes;
        std::array<std::atomic<acl::fiber_mutex*>, scan_lock_count> scan_locks;
        store();
        ~store();
    };
    struct location { size_t shard; size_t bucket; };
    std::vector<std::unique_ptr<store>> stores_;
    location locate(const std::string& key) const;
    acl::fiber_mutex& scan_lock(const location& position);
    scan_index* index(const location& position, bool create);
    const owner_value* add_owner(store& shard, scan_index& index,
        const std::string& key, const record_ptr& value);
    record_ptr find(const std::string& key,
        const owner_value** canonical = nullptr);
    batch_status commit(const std::vector<batch_entry>& roots,
        const std::vector<page_change>& pages, bool preserve_pages,
        prepare_callback* ready = nullptr);
    struct commit_state;
};

} // namespace pkv
#endif
