#include "stdafx.h"
#include "slave_watcher.h"
#include "rpc/exec_batch.h"

namespace pkv {

void slave_watcher::abort_exec() {
    std::lock_guard<std::mutex> guard(queue_lock_);
    if (exec_active_) exec_failed_ = true;
}

bool slave_watcher::begin_exec() {
    std::lock_guard<std::mutex> guard(queue_lock_);
    if (exec_active_) return false;
    exec_active_ = true; exec_capture_ = !eof_ && clients_count_.load() != 0;
    exec_failed_ = false; exec_bytes_ = exec_records_ = 0;
    exec_staged_bytes_ = exec_staged_records_ = 0;
    exec_expanded_bytes_ = exec_actions_ = 0;
    exec_preparing_ = false; ++exec_generation_;
    collecting_.store(exec_capture_);
    return true;
}

bool slave_watcher::capture_pages_locked(unsigned database,
    const std::vector<db::batch_entry>& roots,
    const std::vector<page_change>& pages, const page_batch_size* measured) {
    if (!exec_capture_ || exec_failed_) return true;
    if (exec_preparing_) { exec_failed_ = true; return false; }
    page_batch_size local;
    if (!measured && !page_batch::measure(database, roots, pages, local)) {
        exec_failed_ = true; return false;
    }
    const auto& size = measured ? *measured : local;
    const bool first = exec_pages_.empty() && exec_entries_.empty();
    const auto bytes = size.bytes + (first ? 56 : 0);
    const auto records = size.records + (first ? 2 : 0);
    const auto expanded = size.expanded_bytes +
        (first ? page_batch::group_storage_bytes : 0);
    const auto actions = size.actions + (first ? 2 : 0);
    if (bytes > page_batch::max_bytes - exec_bytes_ ||
        records > page_batch::max_records - exec_records_ ||
        size.bytes > page_batch::max_bytes - exec_staged_bytes_ ||
        size.records > page_batch::max_records - exec_staged_records_ ||
        expanded > page_batch::max_expanded_bytes - exec_expanded_bytes_ ||
        actions > page_batch::max_expanded_records - exec_actions_ ||
        !budget_.reserve_encoded(bytes, records, expanded, actions)) {
        exec_failed_ = true; return false;
    }
    exec_bytes_ += bytes; exec_records_ += records;
    exec_expanded_bytes_ += expanded; exec_actions_ += actions;
    exec_staged_bytes_ += size.bytes; exec_staged_records_ += size.records;
    try {
        auto batch = page_batch::capture(database, roots, pages);
        if (batch) {
            if (exec_entries_.empty()) exec_pages_.push_back(std::move(batch));
            else exec_entries_.emplace_back(std::move(batch));
            return true;
        }
    } catch (const std::bad_alloc&) {}
    exec_failed_ = true; return false;
}

void slave_watcher::capture_root_locked(unsigned database,
    const std::string& key, const std::string& value,
    message_oper_t operation) {
    if (!exec_capture_ || exec_failed_) return;
    if (value.size() > page_transaction::max_value_bytes ||
        (operation != message_oper_set && operation != message_oper_del)) {
        exec_failed_ = true; return;
    }
    try {
        std::vector<db::batch_entry> roots;
        roots.emplace_back(key, true, value);
        roots.back().action = operation == message_oper_set ?
            db::update_action::write : db::update_action::erase;
        roots.back().clear_pages = operation == message_oper_set;
        capture_pages_locked(database, roots, {});
    } catch (const std::bad_alloc&) { exec_failed_ = true; }
}

void slave_watcher::end_exec() {
    std::vector<shared_client> rejected;
    {
        std::lock_guard<std::mutex> guard(queue_lock_);
        bool queued = false;
        if (exec_preparing_) exec_failed_ = true;
        if (!exec_failed_ && !eof_ &&
            (!exec_pages_.empty() || !exec_entries_.empty()) &&
            clients_count_.load() != 0) {
            try {
                auto group = exec_entries_.empty() ?
                    exec_batch::capture(std::move(exec_pages_)) :
                    exec_batch::capture_ordered(std::move(exec_entries_));
                if (group && group->bytes() == exec_bytes_ &&
                    group->records() == exec_records_ &&
                    group->expanded_bytes() == exec_expanded_bytes_ &&
                    group->actions() == exec_actions_) {
                    std::unique_ptr<kv_message> message(new kv_message(group));
                    std::unique_ptr<publication> item(new publication(
                        message.get(), epoch_));
                    message.release();
                    if (box_.push(item.get())) {
                        item.release(); queued = true;
                    }
                }
                if (!queued) exec_failed_ = true;
            } catch (const std::bad_alloc&) { exec_failed_ = true; }
        }
        if (!queued && exec_records_)
            budget_.release_encoded(exec_bytes_, exec_records_,
                exec_expanded_bytes_, exec_actions_);
        std::vector<std::shared_ptr<const page_batch>>().swap(exec_pages_);
        std::vector<exec_entry>().swap(exec_entries_);
        exec_bytes_ = exec_records_ = 0;
        exec_staged_bytes_ = exec_staged_records_ = 0;
        exec_expanded_bytes_ = exec_actions_ = 0;
        exec_preparing_ = false;
        if (exec_failed_ || (eof_ && exec_capture_)) {
            logger_error("EXEC publication failed; invalidating subscribers");
            rejected = invalidate_locked();
        }
        exec_active_ = exec_capture_ = exec_failed_ = false;
        collecting_.store(false);
    }
    for (const auto& client : rejected) client->disconnect();
}

} // namespace pkv
