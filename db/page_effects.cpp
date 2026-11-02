#include "stdafx.h"
#include "db.h"
#include "page_transaction.h"
#include "db_watcher.h"

namespace pkv {
void db::publish_page_effects(const std::vector<batch_entry>& roots,
    const std::vector<page_change>& pages,
    const std::vector<key_ready_change>& ready) {
    if (ready_registry_) ready_registry_->publish(ready);
    size_t cursor = 0;
    for (const auto& root : roots) {
        while (cursor < pages.size() && pages[cursor].owner < root.key)
            ++cursor;
        if (root.action != update_action::keep)
            track_expiry(root.key, root.action == update_action::erase ?
                nullptr : &root.value);
        if (root.maintenance_only && root.found) continue;
        const bool edited = root.action != update_action::keep ||
            (cursor < pages.size() && pages[cursor].owner == root.key);
        if (edited) invalidate_watch(root.key,
            root.action == update_action::erase &&
                !root.force_watch_invalidation);
    }
}
void db::observe_pages(const std::vector<batch_entry>& roots,
    const std::vector<page_change>& pages) {
    if (!watchers_) return;
    try {
        if (!watchers_->on_pages(roots, pages))
            logger_error("Page observer failed after native commit");
    } catch (const std::bad_alloc&) {
        // Data is committed; observer failure cannot turn it into rollback.
        logger_error("Page observer allocation failed after commit");
    }
}
} // namespace pkv
