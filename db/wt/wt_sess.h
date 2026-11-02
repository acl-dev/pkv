#pragma once
#ifdef HAS_WT
#include "db/page_transaction.h"
#include <wiredtiger.h>

namespace pkv {
class wt;

class wt_sess {
public:
    explicit wt_sess(wt& database);
    ~wt_sess();
    bool open();
    void reset();
    db::sync_status sync();
    db::lookup_status bounded_root(const std::string& owner, std::string& out);
    db::batch_status visit_pages(const std::string& owner,
        const db::page_visitor& visit);
    bool count_roots(uint64_t& out);
    db::scan_status visit_keys(const db::key_visitor& visit);
    db::lookup_status lookup(bool page, const std::string& key,
        std::string& out);
    db::batch_status apply(const std::vector<db::batch_entry>& roots,
        const std::vector<page_change>& pages, bool preserve);
    db::scan_status scan(unsigned bucket, std::vector<std::string>& out);
    static void key(WT_CURSOR* cursor, const std::string& value);
    static int put(WT_CURSOR* cursor, const std::string& name,
        const std::string& value);
    static int remove(WT_CURSOR* cursor, const std::string& name);
    static int seek(WT_CURSOR* cursor, const std::string& prefix);
    static int value(WT_CURSOR* cursor, std::string& out, bool key = false);
private:
    wt& database_;
    WT_SESSION* session_ = nullptr;
    WT_CURSOR* roots_ = nullptr;
    WT_CURSOR* pages_ = nullptr;
    WT_CURSOR* end_ = nullptr;
    WT_CURSOR* index_ = nullptr;
    bool transaction_ = false;
    int clear(const std::string& owner);
    int stage(const std::vector<db::batch_entry>& roots,
        const std::vector<page_change>& pages, bool preserve);
};
} // namespace pkv
#endif
