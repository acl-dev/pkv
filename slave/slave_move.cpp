#include "stdafx.h"
#include "slave_move.h"
#include <cstdlib>

namespace pkv {
slave_watcher::prepared_move::~prepared_move() {
    if (!reserved_) return;
    std::lock_guard<std::mutex> guard(owner_.queue_lock_);
    owner_.budget_.release_encoded(bytes_, records_,
        expanded_bytes_, actions_);
    if (grouped_ && generation_ == owner_.exec_generation_)
        owner_.exec_preparing_ = false;
}
void slave_watcher::prepared_move::publish() noexcept {
    if (!reserved_) return;
    std::vector<shared_client> rejected;
    {
        std::lock_guard<std::mutex> guard(owner_.queue_lock_);
        if (grouped_) {
            const auto prefix = owner_.exec_entries_.empty() ?
                owner_.exec_pages_.size() : owner_.exec_entries_.size();
            if (!owner_.exec_active_ || !owner_.exec_preparing_ ||
                generation_ != owner_.exec_generation_ || prefix != prefix_)
                std::abort();
            owner_.exec_preparing_ = false;
            if (owner_.exec_failed_ || owner_.eof_ || epoch_ != owner_.epoch_) {
                owner_.exec_failed_ = true;
                owner_.budget_.release_encoded(bytes_, records_,
                    expanded_bytes_, actions_);
            } else {
                if (converted_.empty()) {
                    if (owner_.exec_entries_.size() ==
                        owner_.exec_entries_.capacity()) std::abort();
                    owner_.exec_entries_.push_back(std::move(entry_));
                } else {
                    owner_.exec_entries_.swap(converted_);
                    owner_.exec_pages_.clear();
                }
                owner_.exec_bytes_ += bytes_; owner_.exec_records_ += records_;
                owner_.exec_staged_bytes_ += charge_;
                owner_.exec_staged_records_ += count_;
                owner_.exec_expanded_bytes_ += expanded_bytes_;
                owner_.exec_actions_ += actions_;
            }
            reserved_ = false; return;
        }
        bool queued = false;
        if (!owner_.eof_ && epoch_ == owner_.epoch_ &&
            owner_.clients_count_.load()) {
            try { queued = owner_.box_.push(item_.get()); }
            catch (const std::bad_alloc&) {}
            if (queued) item_.release();
            else rejected = owner_.invalidate_locked();
        } else if (owner_.eof_ && epoch_ == owner_.epoch_)
            rejected = owner_.invalidate_locked();
        if (!queued) owner_.budget_.release_encoded(bytes_, records_,
            expanded_bytes_, actions_);
        reserved_ = false;
    }
    for (const auto& client : rejected) client->disconnect();
}
} // namespace pkv
