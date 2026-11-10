//
// Created by zsx on 2023/9/24.
//

#include "stdafx.h"
#include "db_watcher.h"
#include "rpc/page_measure.h"
#include "rpc/page_wire.h"

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

bool db_watchers::observes_reads() const {
    for (const auto* watcher : watchers_)
        if (watcher->observes_reads()) return true;
    return false;
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

bool db_watchers::requires_page_payload() const {
    for (const auto watcher : watchers_) {
        if (watcher->requires_page_payload()) return true;
    }
    return false;
}

bool db_watchers::small_page_update_fits(size_t key_size) const {
    // A root and up to two bounded pages fit without computing wire sizes.
    // The full measure still handles compact encodings and other batches.
    static_assert(5 * page_wire::header_bytes + 32 +
        3 * (page_batch::max_key_bytes + page_transaction::max_value_bytes) <=
        page_batch::max_bytes && page_batch::max_records >= 5,
        "small page preflight must fit the plain wire budget");
    return database_ < page_batch::max_databases && key_size <= page_batch::max_key_bytes;
}

bool db_watchers::can_overwrite_string(size_t key_size) const {
    return small_page_update_fits(key_size) && supports_pages() && !requires_page_payload();
}

bool db_watchers::can_pages(const std::vector<db::batch_entry>& roots,
    const std::vector<page_change>& pages) const {
    if (!supports_pages()) return false;
    if (roots.size() == 1 && pages.size() <= 2 && small_page_update_fits(roots.front().key.size())) {
        const auto& root = roots.front();
        if ((root.action != db::update_action::keep || !pages.empty()) &&
            page_measure::valid_root(root, nullptr)) {
            uint64_t previous = 0;
            bool valid = true;
            for (const auto& page : pages) {
                valid = page.owner == root.key &&
                    page_measure::valid_page(root, page, previous);
                if (!valid) break;
                previous = page.id;
            }
            if (valid) return true;
        }
    }
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
