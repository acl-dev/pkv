#include "stdafx.h"
#include "family_workspace_internal.h"
#include "coder/record_codec.h"

namespace pkv {
void family_workspace::record_ready(owner& value) {
    const auto rank = state_->rank++;
    record_header header;
    const auto status = value.database->value_codec().stored_header(
        value.value, header);
    if (status == codec_status::out_of_memory) throw std::bad_alloc();
    if (status != codec_status::ok || (header.expires_at_ms >= 0 &&
        header.expires_at_ms <= unix_time_ms())) return;
    if (header.kind == value_kind::list && value.list_rank == SIZE_MAX)
        value.list_rank = rank;
    if (header.kind == value_kind::zset && value.zset_rank == SIZE_MAX)
        value.zset_rank = rank;
}
db::batch_status family_workspace::stage(
    std::shared_ptr<const page_batch> batch) {
    if (state_->failed || state_->finished) return db::batch_status::aborted;
    if (!batch) return reject(db::batch_status::aborted);
    const auto& size = batch->size();
    if (!charge(size.bytes, size.records))
        return reject(db::batch_status::error);
    try {
        // Once retained, all borrowed page pointers survive until discard.
        // A partial PRIVATE update is harmless: any error poisons the group.
        state_->batches.push_back(batch);
        for (const auto& root : batch->roots()) {
            owner* value;
            const auto status = touch(batch->database(), root.key, value);
            if (status != db::batch_status::ok) return reject(status);
            value->dirty = true;
            if (root.action == db::update_action::keep) continue;
            const auto next = root.action == db::update_action::write ?
                root.value.size() : 0;
            const auto previous = value->value.size();
            if (next > state_->byte_limit -
                (state_->cache_bytes - previous))
                return reject(db::batch_status::error);
            if (root.action == db::update_action::erase) {
                value->value.clear(); value->data.pages.clear();
                value->data.origin = nullptr;
                value->present = false; value->logical = true;
            } else {
                if (root.clear_pages || !value->present) {
                    value->data.pages.clear(); value->data.origin = nullptr;
                }
                value->value = root.value;
                if (!value->present || !root.maintenance_only)
                    value->logical = value->history = true;
                value->present = true;
            }
            state_->cache_bytes = state_->cache_bytes - previous + next;
            if (root.action == db::update_action::write) record_ready(*value);
        }
        size_t position = 0;
        for (const auto& page : batch->pages()) {
            while (batch->roots()[position].key < page.owner) ++position;
            const auto& root = batch->roots()[position];
            auto& value = state_->owners.at(
                address(batch->database(), page.owner));
            if (page.action == db::update_action::write && !value.present)
                return reject(db::batch_status::aborted);
            if (value.present) {
                value.data.pages[page.id] = &page;
            }
            if (!root.maintenance_only) {
                value.logical = true;
                if (root.action != db::update_action::erase)
                    value.history = true;
            }
        }
        state_->bytes += size.bytes; state_->records += size.records;
        return db::batch_status::ok;
    } catch (...) { return reject(db::batch_status::error); }
}
} // namespace pkv
