#include "stdafx.h"
#include "family_workspace_internal.h"
#include "common/key_wait.h"

namespace pkv {
db::batch_status family_workspace::materialize(
    std::vector<family_edit>& out) {
    std::map<const owner*, size_t> positions;
    for (const auto& entry : state_->owners)
        if (entry.second.dirty)
            positions.emplace(&entry.second, positions.size());
    out.reserve(positions.size());
    const auto waiting = key_wait_registry::waiting_types();
    for (const auto& entry : state_->owners) {
        const auto& value = entry.second;
        if (!value.dirty) continue;
        family_edit edit;
        edit.database = entry.first.first; edit.key = entry.first.second;
        edit.expected_found = value.original_found;
        edit.expected_root = value.original;
        edit.action = value.present ? db::update_action::write :
            db::update_action::erase;
        edit.value = value.present ? value.value : "";
        edit.maintenance_only = !value.logical;
        edit.force_watch_invalidation = value.history;
        if (waiting & (1U << static_cast<unsigned>(key_wait_kind::list)))
            edit.ready_rank = value.list_rank;
        if (waiting & (1U << static_cast<unsigned>(key_wait_kind::zset)))
            edit.ready_rank = std::min(edit.ready_rank, value.zset_rank);
        if (value.present) {
            if (value.data.origin) {
                auto found = positions.find(value.data.origin);
                if (found == positions.end()) return db::batch_status::error;
                edit.family = found->second;
            }
            for (const auto& page : value.data.pages)
                edit.pages.emplace_back(edit.key, page.first,
                    page.second->value, page.second->action);
        }
        out.push_back(std::move(edit));
    }
    return db::batch_status::ok;
}
db::batch_status family_workspace::commit(
    const family_plan::observer& observe) {
    if (state_->failed || state_->finished) return db::batch_status::aborted;
    state_->finished = true;
    try {
        std::vector<family_edit> edits;
        auto status = materialize(edits);
        if (status == db::batch_status::ok && !edits.empty())
            status = family_plan::apply(state_->pool, edits, observe);
        if (status != db::batch_status::ok) state_->failed = true;
        return status;
    } catch (...) { return reject(db::batch_status::error); }
}
} // namespace pkv
