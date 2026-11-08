#include "stdafx.h"
#include "db.h"
#include "page_transaction.h"
#include "db_watcher.h"

namespace pkv {
void db::publish_page_effects(const std::vector<batch_entry>& roots,
    const std::vector<page_change>& pages,
    const std::vector<key_ready_change>& ready, const long long* deadline) {
    if (ready_registry_ && !ready.empty()) ready_registry_->publish(ready);
    size_t cursor = 0;
    for (const auto& root : roots) {
        if (deadline && root.action == update_action::write)
            track_deadline(root.key, *deadline);
        else if (root.action != update_action::keep)
            track_expiry(root.key, root.action == update_action::erase ?
                nullptr : &root.value);
        if (root.maintenance_only && root.found) continue;
        if (root.action == update_action::keep) {
            // A root write already establishes the edit without scanning pages.
            while (cursor < pages.size() && pages[cursor].owner < root.key)
                ++cursor;
            if (cursor == pages.size() || pages[cursor].owner != root.key)
                continue;
        }
        invalidate_watch(root.key,
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
