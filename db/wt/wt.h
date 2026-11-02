#pragma once
#include "../db.h"
#include <mutex>
#include <atomic>
#ifdef HAS_WT

typedef struct __wt_connection WT_CONNECTION;
namespace pkv {
class wt_sess;

class wt {
public:
    explicit wt(size_t session_max = 10000);
    ~wt();
    bool open(const char* path);
    bool close();
    db::sync_status sync();
    db::lookup_status bounded_root(const std::string& owner, std::string& out);
    db::batch_status visit_pages(const std::string& owner,
        const db::page_visitor& visit);
    db::lookup_status lookup(const std::string& key, std::string& out);
    db::lookup_status page(const std::string& owner, uint64_t id,
        std::string& out);
    db::batch_status apply(const std::vector<db::batch_entry>& roots,
        const std::vector<page_change>& pages, bool preserve);
    db::scan_status scan(unsigned bucket, std::vector<std::string>& out);
    db::scan_status visit_keys(const db::key_visitor& visit);
    uint64_t key_count() const {
        return root_count_.load(std::memory_order_relaxed);
    }
    WT_CONNECTION* get_db() const { return db_; }
private:
    WT_CONNECTION* db_ = nullptr;
    std::atomic<uint64_t> root_count_{0};
    const size_t session_max_;
    std::vector<wt_sess*> sessions_;
    std::mutex lock_;
    struct lease;
    wt_sess* get_session();
    void put_session(wt_sess* session);
    bool bootstrap();
};
} // namespace pkv
#endif
