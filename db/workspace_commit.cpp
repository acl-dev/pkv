#include "stdafx.h"
#include "page_workspace.h"
#include "db_watcher.h"

namespace pkv {
void page_workspace::prepare_commit(std::vector<batch_entry>& roots,
    std::vector<page_change>& pages) const {
    roots.reserve(owners_.size());
    size_t count = 0;
    for (const auto& owner : owners_)
        for (const auto& page : owner.second.pages)
            if (page.second.page &&
                page.second.generation == owner.second.generation) ++count;
    pages.reserve(count);
    for (const auto& owner : owners_) {
        const auto& state = owner.second;
        const auto action = state.root ? state.root->action :
            update_action::keep;
        roots.emplace_back(owner.first, state.initial_found,
            action == update_action::write ? state.root->value : "");
        auto& root = roots.back();
        root.action = action;
        root.clear_pages = state.reset && action == update_action::write;
        root.maintenance_only = !state.logical;
        root.force_watch_invalidation = state.history;
        for (const auto& page : state.pages)
            if (page.second.page && page.second.generation == state.generation)
                pages.push_back(*page.second.page);
    }
}
bool db::can_observe_page_sequence(const std::vector<page_update>& updates) {
    if (watchers_)
        for (const auto& update : updates)
            if (!watchers_->can_pages(update.roots, update.pages)) return false;
    return true;
}
void db::observe_page_sequence(const std::vector<page_update>& updates) {
    if (!watchers_) return;
    for (const auto& update : updates) {
        try {
            if (!watchers_->on_pages(update.roots, update.pages))
                logger_error("Sequence observer failed after commit");
        } catch (const std::bad_alloc&) {
            logger_error("Sequence observer allocation failed after commit");
        }
    }
}
db::batch_status db::commit_workspace(std::vector<batch_entry>& roots,
    std::vector<page_change>& pages,
    const std::vector<page_update>& observations) {
    execution_guard admission(true);
    if (!can_observe_page_sequence(observations))
        return batch_status::unsupported;
    const auto status = commit_pages(roots, pages, false,
        observations.data(), observations.size());
    if (status == batch_status::ok) observe_page_sequence(observations);
    return status;
}
} // namespace pkv
