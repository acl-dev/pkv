#include "stdafx.h"
#include "family_workspace_internal.h"
#include "database_pool.h"
#include "rpc/move_intent.h"

namespace pkv {
db::batch_status family_workspace::stage(const family_move& move) {
    if (state_->failed || state_->finished) return db::batch_status::aborted;
    if (move.source == move.target || move.source >= state_->pool.size() ||
        move.target >= state_->pool.size() ||
        move.source >= page_batch::max_databases ||
        move.target >= page_batch::max_databases ||
        move.key.size() > page_batch::max_key_bytes ||
        move.source_root.size() > page_transaction::max_value_bytes ||
        move.target_root.size() > page_transaction::max_value_bytes ||
        (!move.target_found && !move.target_root.empty()))
        return reject(db::batch_status::aborted);
    // Normalized staging cost, not a negotiated/encoded wire format.
    const size_t bytes = 64 + 2 * move.key.size() + move.source_root.size() +
        move.target_root.size();
    const auto expanded = move_intent::expanded_bytes(move);
    if (!charge(bytes, 3) || expanded > page_batch::max_expanded_bytes -
        state_->expanded_bytes ||
        3 > page_batch::max_expanded_records - state_->actions)
        return reject(db::batch_status::error);
    try {
        owner *source, *target;
        auto status = touch(move.source, move.key, source);
        if (status != db::batch_status::ok) return reject(status);
        status = touch(move.target, move.key, target);
        if (status != db::batch_status::ok) return reject(status);
        if (!source->present || source->value != move.source_root ||
            target->present != move.target_found ||
            (target->present && target->value != move.target_root))
            return reject(db::batch_status::aborted);
        const auto* origin = source->data.origin;
        if (origin && !origin->database->dbsupports_family_transfer(
            *target->database)) return reject(db::batch_status::unsupported);
        state_->cache_bytes -= target->value.size();
        target->value = std::move(source->value); source->value.clear();
        target->data.pages.swap(source->data.pages);
        target->data.origin = source->data.origin;
        source->data.pages.clear(); source->data.origin = nullptr;
        source->present = false; source->dirty = source->logical = true;
        target->present = true;
        target->dirty = target->logical = target->history = true;
        record_ready(*target);
        state_->bytes += bytes; state_->records += 3;
        state_->expanded_bytes += expanded; state_->actions += 3;
        return db::batch_status::ok;
    } catch (...) { return reject(db::batch_status::error); }
}
} // namespace pkv
