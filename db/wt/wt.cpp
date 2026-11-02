#include "stdafx.h"
#ifdef HAS_WT
#include "wt.h"
#include "db/key_count.h"
#include "wt_sess.h"
#include "db/disk_address.h"

namespace pkv {

wt::wt(size_t session_max) : session_max_(session_max) {}
wt::~wt() { close(); }
bool wt::close() {
    for (auto* session : sessions_) delete session;
    sessions_.clear();
    auto* database = db_; db_ = nullptr;
    const int result = database ? database->close(database, nullptr) : 0;
    if (result) logger_error("WiredTiger shutdown failed: %s",
        wiredtiger_strerror(result));
    return result == 0;
}
bool wt::open(const char* path) {
    const char* config = "create,log=(enabled=true),cache_size=128MB,"
        "session_max=256";
    const int result = wiredtiger_open(path, nullptr, config, &db_);
    if (result) {
        logger_error("WiredTiger open failed: %s", wiredtiger_strerror(result));
        return false;
    }
    if (!bootstrap()) return false;
    std::unique_ptr<wt_sess> session(new wt_sess(*this));
    uint64_t count;
    if (!session->open() || !session->count_roots(count)) return false;
    root_count_.store(count, std::memory_order_relaxed);
    return true;
}
wt_sess* wt::get_session() {
    {
        std::lock_guard<std::mutex> guard(lock_);
        if (!sessions_.empty()) {
            auto* session = sessions_.back(); sessions_.pop_back();
            return session;
        }
    }
    std::unique_ptr<wt_sess> session(new wt_sess(*this));
    return session->open() ? session.release() : nullptr;
}
void wt::put_session(wt_sess* session) {
    session->reset();
    try {
        std::lock_guard<std::mutex> guard(lock_);
        if (sessions_.size() < session_max_) {
            sessions_.push_back(session); return;
        }
    } catch (const std::bad_alloc&) {}
    delete session;
}
struct wt::lease {
    wt& owner;
    wt_sess* session;
    explicit lease(wt& database)
    : owner(database), session(database.get_session()) {}
    ~lease() { if (session) owner.put_session(session); }
};

db::sync_status wt::sync() {
    if (!db_) return db::sync_status::error;
    try {
        lease borrowed(*this);
        return borrowed.session ? borrowed.session->sync() :
            db::sync_status::error;
    } catch (const std::bad_alloc&) { return db::sync_status::error; }
}

db::lookup_status wt::bounded_root(const std::string& owner,
    std::string& out) {
    if (!db_) return db::lookup_status::error;
    try {
        lease borrowed(*this);
        return borrowed.session ? borrowed.session->bounded_root(owner, out)
            : db::lookup_status::error;
    } catch (const std::bad_alloc&) { return db::lookup_status::error; }
}

db::batch_status wt::visit_pages(const std::string& owner,
    const db::page_visitor& visit) {
    if (!db_) return db::batch_status::error;
    try {
        lease borrowed(*this);
        return borrowed.session ? borrowed.session->visit_pages(owner, visit)
            : db::batch_status::error;
    } catch (const std::bad_alloc&) { return db::batch_status::error; }
}

db::lookup_status wt::lookup(const std::string& key, std::string& out) {
    try {
        lease borrowed(*this);
        return borrowed.session ? borrowed.session->lookup(false,
            std::string(1, '\1') + key, out) : db::lookup_status::error;
    } catch (const std::bad_alloc&) { return db::lookup_status::error; }
}
db::lookup_status wt::page(const std::string& owner, uint64_t id,
    std::string& out) {
    try {
        lease borrowed(*this);
        return borrowed.session ? borrowed.session->lookup(true,
            disk_address::page(owner, id), out) : db::lookup_status::error;
    } catch (const std::bad_alloc&) { return db::lookup_status::error; }
}
db::batch_status wt::apply(const std::vector<db::batch_entry>& roots,
    const std::vector<page_change>& pages, bool preserve) {
    try {
        lease borrowed(*this);
        const auto status = borrowed.session ?
            borrowed.session->apply(roots, pages, preserve) :
            db::batch_status::error;
        if (status == db::batch_status::ok) {
            const auto delta = root_count_delta(roots);
            if (delta) root_count_.fetch_add(static_cast<uint64_t>(delta),
                std::memory_order_relaxed);
        }
        return status;
    } catch (const std::bad_alloc&) { return db::batch_status::error; }
}
db::scan_status wt::scan(unsigned bucket, std::vector<std::string>& out) {
    try {
        lease borrowed(*this);
        return borrowed.session ? borrowed.session->scan(bucket, out) :
            db::scan_status::error;
    } catch (const std::bad_alloc&) { return db::scan_status::error; }
}

db::scan_status wt::visit_keys(const db::key_visitor& visit) {
    try {
        lease borrowed(*this);
        return borrowed.session ? borrowed.session->visit_keys(visit) :
            db::scan_status::error;
    } catch (const std::bad_alloc&) { return db::scan_status::error; }
}

} // namespace pkv
#endif
