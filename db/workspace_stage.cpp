#include "stdafx.h"
#include "page_workspace.h"

namespace pkv {
db::batch_status page_workspace::stage(
    std::shared_ptr<const page_batch> batch) {
    if (!batch || bytes_ > byte_limit_ || records_ > record_limit_ ||
        batch->size().bytes > byte_limit_ - bytes_ ||
        batch->size().records > record_limit_ - records_ ||
        batch->size().expanded_bytes >
            page_batch::max_expanded_bytes - expanded_bytes_ ||
        batch->size().actions > page_batch::max_expanded_records - actions_)
        return batch_status::error;
    // Prepare all allocating operations first. Existing pointers/generations
    // stay untouched until every index node and history slot is available.
    std::vector<decltype(owners_)::iterator> added_owners;
    std::vector<position> added_pages;
    std::vector<position> positions;
    try {
        added_owners.reserve(batch->roots().size());
        added_pages.reserve(batch->pages().size());
        positions.reserve(batch->pages().size());
        if (batches_.size() == batches_.capacity()) {
            const auto capacity = batches_.capacity();
            batches_.reserve(capacity ? capacity * 2 : 8);
        }
        for (const auto& root : batch->roots()) {
            auto found = owners_.lower_bound(root.key);
            if (found == owners_.end() || found->first != root.key) {
                found = owners_.emplace_hint(found, root.key, owner_state());
                added_owners.push_back(found);
                found->second.initial_found = root.found;
                found->second.present = root.found;
            }
        }
        for (const auto& page : batch->pages()) {
            auto& owner = owners_.find(page.owner)->second;
            auto found = owner.pages.lower_bound(page.id);
            if (found == owner.pages.end() || found->first != page.id) {
                found = owner.pages.emplace_hint(found, page.id, page_state());
                added_pages.emplace_back(&owner, found);
            }
            positions.emplace_back(&owner, found);
        }
    } catch (const std::bad_alloc&) {
        for (const auto& item : added_pages)
            item.first->pages.erase(item.second);
        for (auto it : added_owners) owners_.erase(it);
        return batch_status::error;
    }
    publish_indexes(*batch, positions);
    bytes_ += batch->size().bytes; records_ += batch->size().records;
    expanded_bytes_ += batch->size().expanded_bytes;
    actions_ += batch->size().actions;
    batches_.push_back({std::move(batch),
        key_wait_registry::waiting_types()});
    return batch_status::ok;
}
void page_workspace::publish_indexes(const page_batch& batch,
    const std::vector<position>& positions) {
    for (const auto& root : batch.roots()) {
        auto& state = owners_.find(root.key)->second;
        if (root.action == update_action::keep) continue;
        if (root.clear_pages || root.action == update_action::erase ||
            (state.root && state.root->action == update_action::erase)) {
            ++state.generation; state.reset = true;
        }
        if (root.action == update_action::erase) {
            if (state.present) --root_count_delta_;
            state.present = false; state.logical = true;
        } else {
            if (!state.present) ++root_count_delta_;
            if (!state.present || !root.maintenance_only)
                state.logical = state.history = true;
            state.present = true;
        }
        state.root = &root;
    }
    size_t index = 0, owner = 0;
    for (const auto& page : batch.pages()) {
        const auto& location = positions[index++];
        auto& state = location.second->second;
        state.page = &page;
        state.generation = location.first->generation;
        while (batch.roots()[owner].key < page.owner) ++owner;
        const auto& declaration = batch.roots()[owner];
        if (!declaration.maintenance_only) {
            location.first->logical = true;
            if (declaration.action != update_action::erase)
                location.first->history = true;
        }
    }
}
} // namespace pkv
