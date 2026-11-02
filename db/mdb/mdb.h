//
// Created by shuxin zheng on 2023/8/21.
//

#pragma once

#include "../db.h"
#include <unordered_map>
#include <array>
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
    bool dbinline_strings() const override { return true; }
    inline_status dbread_inline(const std::string& key, int64_t now,
        std::string& out) override;

    // @override
    bool dbdel(const std::string& key) override;
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
    batch_status dbapply_pages(const std::vector<batch_entry>& roots,
        const std::vector<page_change>& pages) override;


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
    std::vector<std::unique_ptr<lock_t>> locks_;
    static const size_t scan_buckets = 256;
    using page_map = std::map<uint64_t, std::string>;
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
        std::array<std::unique_ptr<page_family>, scan_buckets> pages;
    };
    struct location { size_t shard; size_t bucket; };
    location locate(const std::string& key) const;
    // Caller holds the shard lock; bucket objects remain stable once created.
    root_map* find_store(const location& position, bool create);
    bool add_root(store& shard, root_map& bucket, const std::string& key,
        const std::string& value);
    void erase_root(store& shard, root_map& bucket, root_map::iterator node);
    page_family* find_page_family(const location& position, bool create);
    void erase_pages(const std::string& owner);
    batch_status commit(const std::vector<batch_entry>& roots,
        const std::vector<page_change>& pages, bool preserve_pages,
        prepare_callback* ready = nullptr);
    struct commit_state;
    std::vector<std::unique_ptr<store>> stores_;
};

} // namespace pkv
