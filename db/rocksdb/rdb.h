#pragma once
#include "../db.h"

#ifdef HAS_ROCKSDB
namespace rocksdb { class DB; }

namespace pkv {

class rdb : public db {
public:
    rdb();
    ~rdb() override;
    const char* get_dbtype() const override { return "rdb"; }
protected:
    bool dbopen(const char* paths) override;
    bool dbopen_paths(const std::vector<std::string>& paths) override;
    bool dbclose_for_exit() override;
    bool dbset(const std::string& key, const std::string& value) override;
    bool dbget(const std::string& key, std::string& value) override;
    lookup_status dblookup(const std::string& key, std::string& out) override;
    bool dbdel(const std::string& key) override;
    db_cursor* dbcreate_cursor() override;
    bool dbscan(size_t idx, db_cursor& cursor,
        std::vector<std::string>& keys, size_t max) override;
    count_status dbkey_count(uint64_t& out) override;
    scan_status dbvisit_keys(const key_visitor& visit) override;
    scan_status dbscan_keys(unsigned long long cursor, size_t count,
        std::vector<std::string>& keys, unsigned long long& next) override;
    size_t dbsize() const override { return stores_.size(); }
    lookup_status dblookup_bounded_root(const std::string& owner,
        std::string& out) override;
    bool dbsupports_page_stream() const override { return true; }
    batch_status dbvisit_pages(const std::string& owner,
        const page_visitor& visit) override;
    bool dbsupports_sync() const override { return true; }
    sync_status dbsync_key(const std::string& owner) override;
    bool dbsupports_pages() const override { return true; }
    bool dbatomic_workspace() const override { return stores_.size() == 1; }
    lookup_status dblookup_page(const std::string& owner, uint64_t id,
        std::string& out) override;
    batch_status dbapply(const std::vector<batch_entry>& roots) override;
    batch_status dbapply_pages(const std::vector<batch_entry>& roots,
        const std::vector<page_change>& pages) override;
private:
    struct store;
    std::vector<std::unique_ptr<store>> stores_;
    size_t locate(const std::string& key) const;
    bool open_one(const std::string& path);
    bool index(store& storage);
    bool count_roots(store& storage);
    batch_status commit(const std::vector<batch_entry>& roots,
        const std::vector<page_change>& pages, bool preserve_pages);
};

} // namespace pkv
#endif
