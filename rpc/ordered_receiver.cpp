#include "ordered_receiver.h"
#include "move_intent.h"
#include "page_wire.h"
#include <new>

namespace pkv {
void ordered_receiver::reset() {
    std::vector<exec_entry>().swap(entries_); pages_.reset();
    expected_entries_ = expected_bytes_ = expected_records_ = 0;
    bytes_ = records_ = 0; active_ = failed_ = false;
    expanded_bytes_ = page_batch::group_storage_bytes; actions_ = 2;
    protocol_ = 0;
}
ordered_receiver::result ordered_receiver::reject() {
    reset(); failed_ = true; return result::error;
}
bool ordered_receiver::begin(const page_frame& frame) {
    if (frame.operation != page_operation::exec_begin || frame.database ||
        !frame.key.empty() || frame.id || frame.value.size() != 16)
        return false;
    const auto count = page_wire::get32(frame.value.data());
    const auto records = page_wire::get32(frame.value.data() + 4);
    const auto bytes = page_wire::get64(frame.value.data() + 8);
    if (!count || records > page_batch::max_records ||
        uint64_t(records) < 2 + uint64_t(count) ||
        bytes > page_batch::max_bytes || bytes < 56 + uint64_t(count) * 36)
        return false;
    protocol_ = frame.protocol;
    entries_.reserve(count);
    expected_entries_ = count; expected_records_ = records;
    expected_bytes_ = static_cast<size_t>(bytes);
    bytes_ = 36; records_ = 1; active_ = true;
    return true;
}
bool ordered_receiver::nested_begin(const page_frame& frame) const {
    if (frame.operation != page_operation::begin) return true;
    if (pages_.active() || frame.value.size() != 16 ||
        entries_.size() >= expected_entries_) return false;
    const auto roots = page_wire::get32(frame.value.data());
    const auto pages = page_wire::get32(frame.value.data() + 4);
    const auto bytes = page_wire::get64(frame.value.data() + 8);
    const uint64_t wire_pages = frame.protocol == 5 ?
        (uint64_t(pages) + page_batch::max_erase_ids - 1) /
            page_batch::max_erase_ids : pages;
    return bytes <= expected_bytes_ - bytes_ - 20 &&
        2 + uint64_t(roots) + pages <=
            page_batch::max_expanded_records - actions_ &&
        2 + uint64_t(roots) + wire_pages <=
            expected_records_ - records_ - 1;
}
ordered_receiver::result ordered_receiver::accept(page_frame frame,
    std::shared_ptr<const exec_batch>& out) {
    out.reset();
    if (failed_) return result::error;
    if (frame.protocol != 4 && frame.protocol != 5) return reject();
    if (active_ && protocol_ == 4 && frame.protocol != 4) return reject();
    try {
        if (!active_) return begin(frame) ? result::pending : reject();
        if (frame.operation == page_operation::exec_commit) {
            if (frame.protocol != protocol_ || frame.database ||
                !frame.key.empty() || frame.id ||
                !frame.value.empty() || pages_.active() ||
                entries_.size() != expected_entries_ ||
                records_ + 1 != expected_records_ ||
                bytes_ + 20 != expected_bytes_) return reject();
            auto batch = exec_batch::capture_ordered(std::move(entries_));
            if (!batch || batch->bytes() != expected_bytes_ ||
                batch->records() != expected_records_ ||
                batch->expanded_bytes() != expanded_bytes_ ||
                batch->actions() != actions_) return reject();
            out = std::move(batch); reset(); return result::complete;
        }
        const bool page = frame.operation == page_operation::page_write ||
            frame.operation == page_operation::page_erase;
        const auto bytes = 20 + frame.key.size() + frame.value.size() +
            (page ? 8 : 0);
        if (records_ + 1 >= expected_records_ ||
            bytes > expected_bytes_ - bytes_ - 20 || !nested_begin(frame))
            return reject();
        if (frame.operation == page_operation::family_move) {
            family_move move;
            if (pages_.active() || entries_.size() >= expected_entries_ ||
                !move_intent::decode(frame, move)) return reject();
            const auto expanded = move_intent::expanded_bytes(move);
            if (expanded > page_batch::max_expanded_bytes - expanded_bytes_ ||
                3 > page_batch::max_expanded_records - actions_)
                return reject();
            expanded_bytes_ += expanded; actions_ += 3;
            entries_.emplace_back(std::move(move));
        } else {
            std::shared_ptr<const page_batch> batch;
            const auto status = pages_.accept(std::move(frame), batch,
                page_batch::max_expanded_bytes - expanded_bytes_,
                page_batch::max_expanded_records - actions_);
            if (status == result::error) return reject();
            if (status == result::complete) {
                if (entries_.size() >= expected_entries_) return reject();
                expanded_bytes_ += batch->size().expanded_bytes;
                actions_ += batch->size().actions;
                entries_.emplace_back(std::move(batch));
            }
        }
        bytes_ += bytes; ++records_;
        return result::pending;
    } catch (const std::bad_alloc&) { return reject(); }
}
} // namespace pkv
