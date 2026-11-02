#pragma once
#include "../db.h"
#ifdef HAS_WT

namespace pkv {
class wt;

class wdb : public db {
public:
    explicit wdb(size_t cache_max = 10000);
    ~wdb() override;
protected:
    bool dbopen(const char* paths) override;
    bool dbopen_paths(const std::vector<std::string>& paths) override;
    bool dbclose_for_exit() override;
    bool dbset(const std::string& key, const std::string& value) override;
    bool dbget(const std::string& key, std::string& out) override;
    lookup_status dblookup(const std::string& key, std::string& out) override;
    bool dbdel(const std::string& key) override;
    db_cursor* dbcreate_cursor() override { return nullptr; }
    bool dbscan(size_t, db_cursor&, std::vector<std::string>&, size_t) override;
    count_status dbkey_count(uint64_t& out) override;
    scan_status dbvisit_keys(const key_visitor& visit) override;
    scan_status dbscan_keys(unsigned long long cursor, size_t count,
        std::vector<std::string>& keys, unsigned long long& next) override;
    lookup_status dblookup_bounded_root(const std::string& owner,
        std::string& out) override;
    bool dbsupports_page_stream() const override { return true; }
    batch_status dbvisit_pages(const std::string& owner,
        const page_visitor& visit) override;
    bool dbsupports_sync() const override { return true; }
    sync_status dbsync_key(const std::string& owner) override;
    bool dbsupports_pages() const override { return true; }
    bool dbatomic_workspace() const override { return dbs_.size() == 1; }
    lookup_status dblookup_page(const std::string& owner, uint64_t id,
        std::string& out) override;
    batch_status dbapply(const std::vector<batch_entry>& roots) override;
    batch_status dbapply_pages(const std::vector<batch_entry>& roots,
        const std::vector<page_change>& pages) override;
    size_t dbsize() const override { return dbs_.size(); }
    const char* get_dbtype() const override { return "wdb"; }
private:
    const size_t cache_max_;
    std::vector<std::unique_ptr<wt>> dbs_;
    size_t locate(const std::string& key) const;
    bool open_one(const std::string& path);
    batch_status commit(const std::vector<batch_entry>& roots,
        const std::vector<page_change>& pages, bool preserve);
};
} // namespace pkv
#endif
