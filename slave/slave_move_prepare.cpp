#include "stdafx.h"
#include "slave_move.h"
#include "rpc/move_intent.h"

namespace pkv {
bool slave_watcher::prepare_move(const family_move& move,
    std::unique_ptr<family_publication>& out) {
    out.reset();
    std::lock_guard<std::mutex> guard(queue_lock_);
    if (exec_active_ && (!exec_capture_ || exec_failed_)) return true;
    if (!exec_active_ && !clients_count_.load()) return true;
    if (eof_ || exec_preparing_) return false;
    try {
        family_move translated;
        const bool compact = is_compact_root(move.source_root) ||
            is_compact_root(move.target_root);
        if (compact) {
            translated = move;
            if (!translate_compact_root(translated.source_root) ||
                !translate_compact_root(translated.target_root)) return false;
        }
        const auto& wire_move = compact ? translated : move;
        size_t bytes;
        if (!move_intent::measure(wire_move, bytes)) return false;
        const auto charge = 64 + 2 * wire_move.key.size() + wire_move.source_root.size() +
            wire_move.target_root.size();
        const bool first = exec_pages_.empty() && exec_entries_.empty();
        const auto wire = bytes + ((!exec_active_ || first) ? 56 : 0);
        const size_t records = (!exec_active_ || first) ? 3 : 1;
        const auto expanded = move_intent::expanded_bytes(wire_move) +
            ((!exec_active_ || first) ? page_batch::group_storage_bytes : 0);
        const size_t actions = (!exec_active_ || first) ? 5 : 3;
        if (exec_active_ && (expanded > page_batch::max_expanded_bytes -
            exec_expanded_bytes_ ||
            actions > page_batch::max_expanded_records - exec_actions_ ||
            wire > page_batch::max_bytes - exec_bytes_ ||
            records > page_batch::max_records - exec_records_ ||
            charge > page_batch::max_bytes - exec_staged_bytes_ ||
            3 > page_batch::max_records - exec_staged_records_)) return false;
        std::unique_ptr<prepared_move> prepared(new prepared_move(*this,
            exec_entry(wire_move)));
        prepared->grouped_ = exec_active_;
        prepared->generation_ = exec_generation_; prepared->epoch_ = epoch_;
        prepared->bytes_ = wire; prepared->records_ = records;
        prepared->charge_ = charge; prepared->count_ = 3;
        prepared->expanded_bytes_ = expanded; prepared->actions_ = actions;
        if (exec_active_) {
            prepared->prefix_ = exec_entries_.empty() ? exec_pages_.size() :
                exec_entries_.size();
            if (exec_entries_.empty()) {
                prepared->converted_.reserve(exec_pages_.size() + 1);
                for (const auto& pages : exec_pages_)
                    prepared->converted_.emplace_back(pages);
                prepared->converted_.push_back(prepared->entry_);
            } else if (exec_entries_.size() == exec_entries_.capacity()) {
                exec_entries_.reserve(std::min(page_batch::max_records,
                    std::max(exec_entries_.size() + 1,
                        exec_entries_.capacity() * 2)));
            }
        } else {
            auto group = exec_batch::capture_ordered({prepared->entry_});
            if (!group || group->bytes() != wire ||
                group->records() != records) return false;
            std::unique_ptr<kv_message> message(new kv_message(group));
            prepared->item_.reset(new publication(message.get(), epoch_));
            message.release();
        }
        if (!budget_.reserve_encoded(wire, records, expanded, actions))
            return false;
        prepared->reserved_ = true;
        if (exec_active_) exec_preparing_ = true;
        out = std::move(prepared); return true;
    } catch (const std::bad_alloc&) { return false; }
    catch (const std::runtime_error&) { return false; }
}
} // namespace pkv
