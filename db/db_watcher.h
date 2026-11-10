//
// Created by zsx on 2023/9/24.
//

#pragma once
#include <mutex>
#include "db/page_transaction.h"
#include "db/family_move.h"
#include "db/family_publication.h"

namespace pkv {

class db_watcher {
public:
    db_watcher() = default;
    virtual ~db_watcher() = default;

    virtual bool supports_pages() const { return false; }
    // A false result must remain safe for the current execution admission.
    virtual bool requires_page_payload() const { return true; }
    virtual bool on_pages_database(unsigned,
        const std::vector<db::batch_entry>&,
        const std::vector<page_change>&) { return false; }

    // No root-only fallback can describe ownership of unchanged pages.
    virtual bool prepare_move(const family_move&,
        std::unique_ptr<family_publication>& out) {
        out.reset(); return false;
    }

    virtual bool on_set_database(unsigned database, const std::string& key,
        const std::string& value, bool ok) {
        return database == 0 ? on_set(key, value, ok) : true;
    }
    virtual bool on_get_database(unsigned database, const std::string& key,
        const std::string& value, bool ok) {
        return database == 0 ? on_get(key, value, ok) : true;
    }
    virtual bool on_del_database(unsigned database, const std::string& key, bool
        ok) {
        return database == 0 ? on_del(key, ok) : true;
    }

    virtual bool on_set(const std::string& key, const std::string& value, bool
        ok) = 0;
    virtual bool on_get(const std::string& key, const std::string& value, bool
        ok) = 0;
    virtual bool on_del(const std::string& key, bool ok) = 0;

    // A false result permits native reads to bypass this watcher's on_get.
    virtual bool observes_reads() const { return true; }
};

class db_watchers {
public:
    explicit db_watchers(unsigned database = 0) : database_(database) {}
    ~db_watchers() = default;

    bool on_set(const std::string& key, const std::string& value, bool ok);
    bool on_get(const std::string& key, const std::string& value, bool ok);
    bool on_del(const std::string& key, bool ok);
    bool observes_reads() const;

    bool supports_pages() const;
    bool requires_page_payload() const;
    bool can_overwrite_string(size_t key_size) const;
    bool can_pages(const std::vector<db::batch_entry>& roots,
        const std::vector<page_change>& pages) const;
    bool on_pages(const std::vector<db::batch_entry>& roots,
        const std::vector<page_change>& pages);

    void add_watcher(db_watcher* watcher);

    // Caller owns exclusive admission and the native pair's key locks.
    // Shared observers are prepared once; failure cancels every reservation.
    static bool prepare_move(db_watchers* source, db_watchers* target,
        const family_move& move, std::unique_ptr<family_publication>& out);

private:
    bool small_page_update_fits(size_t key_size) const;
    unsigned database_;
    std::mutex lock_;
    std::vector<db_watcher*> watchers_;
};

} // namespace pkv
