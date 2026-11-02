//
// Created by zsx on 2023/9/24.
//

#include "stdafx.h"
#include "db_watcher.h"
#include "rpc/page_batch.h"

namespace pkv {

bool db_watchers::on_set(const std::string &key, const std::string &value, bool
    ok) {
    for (auto& watcher : watchers_) {
        if (!watcher->on_set_database(database_, key, value, ok)) {
            return false;
        }
    }
    return true;
}

bool db_watchers::on_get(const std::string &key, const std::string &value, bool
    ok) {
    for (auto& watcher : watchers_) {
        if (!watcher->on_get_database(database_, key, value, ok)) {
            return false;
        }
    }
    return true;
}

bool db_watchers::on_del(const std::string &key, bool ok) {
    for (auto& watcher : watchers_) {
        if (!watcher->on_del_database(database_, key, ok)) {
            return false;
        }
    }
    return true;
}

bool db_watchers::supports_pages() const {
    if (watchers_.empty()) {
        return false;
    }
    for (const auto watcher : watchers_) {
        if (!watcher->supports_pages()) {
            return false;
        }
    }
    return true;
}

bool db_watchers::can_pages(const std::vector<db::batch_entry>& roots,
    const std::vector<page_change>& pages) const {
    if (!supports_pages()) return false;
    page_batch_size size;
    return page_batch::measure(database_, roots, pages, size);
}

bool db_watchers::on_pages(const std::vector<db::batch_entry>& roots,
    const std::vector<page_change>& pages) {
    bool ok = true;
    for (auto watcher : watchers_) {
        try {
            if (!watcher->on_pages_database(database_, roots, pages))
                ok = false;
        } catch (const std::bad_alloc&) {
            // Other observers, especially replication, still need the complete
            // committed event. The caller cannot roll back the native commit.
            ok = false;
        }
    }
    return ok;
}

void db_watchers::add_watcher(db_watcher* watcher) {
    lock_.lock();
    watchers_.emplace_back(watcher);
    lock_.unlock();
}

} // namespace pkv
