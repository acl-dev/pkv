//
// Created by shuxin zheng on 2023/8/21.
//

#pragma once

#include "../db.h"
#include "page_index.h"
#include <unordered_map>
#include <array>
#include <atomic>
#include <map>
#include "common/spinlock.h"
#include "common/dense_key_index.h"

namespace pkv {

//using lock_t = acl::thread_mutex;
using lock_t = spinlock;
using map_t  = std::unordered_map<std::string, std::string>;
//using map_t  = std::map<kstring, kstring>;

class mdb : public db {
public:
    mdb(size_t count);
    ~mdb() override;

protected:
    // @override
    bool dbopen(const char* path) override;

    // @override
    bool dbset(const std::string& key, const std::string& value) override;

    // @override
    bool dbget(const std::string& key, std::string& value) override;
    lookup_status dbprobe_root(const std::string& key,
        std::string& scratch) override;
    bool dbinline_strings() const override { return true; }
    inline_status dbread_inline(const std::string& key,
        std::string& out, size_t offset, size_t limit) override;

    // @override
    bool dbdel(const std::string& key) override;
    count_status dbkey_count(uint64_t& out) override;
    scan_status dbkey_at_rank(uint64_t rank, std::string& out,
        bool& found) override;
    scan_status dbsample_key(std::string& out, bool& found) override;
    scan_status dbvisit_keys(const key_visitor& visit) override;
    scan_status dbvisit_root_records(const root_visitor& visit) override;
    scan_status dbscan_keys(unsigned long long cursor, size_t count,
            std::vector<std::string>& keys, unsigned long long& next) override;
    scan_status dbscan_root_page(unsigned long long cursor, size_t count,
        unsigned long long& next, const scan_root_visitor& visit,
        const key_visitor& filter) override;
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
    virtual bool dbborrows_pages() const;
    lookup_status dbinspect_page(const std::string& owner, uint64_t id,
        std::string& scratch, page_inspection& inspection) override;
    bool dbbind_page_reader(const std::string& owner,
        page_reader& reader) override;
    batch_status dbapply_pages(const std::vector<batch_entry>& roots,
        const std::vector<page_change>& pages) override;
    batch_status dbadopt_page(const batch_entry& root, page_change& page) override;
    batch_status dbadopt_page_family(const std::vector<batch_entry>& roots,
        std::vector<page_change>& pages) override;


    // @override
    db_cursor* dbcreate_cursor() override {
        return nullptr;
    }

    // @override
    bool dbscan(size_t, db_cursor&, std::vector<std::string>&, size_t) override;

protected:
    // @override
    NODISCARD size_t dbsize() const override {
        return stores_.size();
    }

    // @override
    NODISCARD const char* get_dbtype() const override {
        return "mdb";
    }

private:
    friend class db;
    friend struct packed_resp_access;
    std::atomic<uint64_t> root_count_{0};
    std::atomic<size_t> sample_shard_bound_{0};
    static constexpr size_t tracked_key_shards = 256;
    static constexpr size_t shard_word_bits = 64;
    std::array<std::atomic<uint64_t>, tracked_key_shards / shard_word_bits> occupied_key_shards_;
    std::vector<std::unique_ptr<lock_t>> locks_;
    static const size_t scan_buckets = 32;
    using page_map = mdb_page_index;
    using page_family = std::unordered_map<std::string, page_map>;
    struct root_record {
        std::string value;
        mutable size_t position = 0;
        explicit root_record(const std::string& bytes) : value(bytes) {}
    };
    using root_map = std::unordered_map<std::string, root_record>;
    struct store {
        dense_key_index<root_map::value_type> keys;
        // Names remain owned only by stable hash nodes; the index borrows them.
        std::array<std::unique_ptr<root_map>, scan_buckets> buckets;
        // Physical pages are never entries in the user-visible key map.
        std::unique_ptr<page_family> pages;
    };
    struct location { size_t shard; size_t bucket; };
    location locate(const std::string& key) const;
    // Caller holds the shard lock; committed erases can release empty buckets.
    root_map* find_store(const location& position, bool create);
    template<class Visit>
    scan_status walk_root_page(unsigned long long cursor, size_t count,
        unsigned long long& next, const Visit& visit);
    void mark_key_shard(size_t shard, bool occupied) noexcept;
    template<class Visit> bool visit_key_shards(const Visit& visit);
    bool add_root(size_t shard, root_map& bucket, const std::string& key,
        const std::string& value);
    void erase_root(size_t shard, root_map& bucket, root_map::iterator node);
    bool erase_root(const location& position, const std::string& key);
    page_family* find_page_family(size_t shard, bool create);
    void erase_pages(const location& position, const std::string& owner);
    void prune_page_family(size_t shard) noexcept;
    bool replace_root(const batch_entry& root);
    inline_status read_inline_hashed(const std::string& key, std::string& out,
        size_t offset, size_t limit, size_t hash);
    const std::string* borrow_root(const std::string& key, size_t hash);
    batch_status edit_bound_list_page(const batch_entry& root,
        const list_page_edit& edit, const page_reader& reader,
        std::string*& bound_root);
    bool replace_page(const batch_entry& root, const page_change& page,
        std::string* adopted = nullptr, page_map* bound = nullptr,
        std::string** bound_root = nullptr);
    batch_status replace_bound_page(const batch_entry& root, page_change& page,
        const page_reader& reader, std::string*& bound_root, bool& adopt);
    static bool reader_has_more_pages(const page_reader& reader, size_t count);
    batch_status apply_bound_pages(const std::vector<batch_entry>& roots,
        const std::vector<page_change>& pages, const page_reader& reader,
        std::string** bound_root);
    batch_status commit(const std::vector<batch_entry>& roots,
        const std::vector<page_change>& pages, bool preserve_pages,
        prepare_callback* ready = nullptr, page_map* bound = nullptr,
        std::string** bound_root = nullptr);
    struct commit_state;
    std::vector<std::unique_ptr<store>> stores_;
};

} // namespace pkv
