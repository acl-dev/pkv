#include "exec_batch.h"
#include "page_sequence.h"
#include "page_wire.h"
#include <new>

namespace pkv {

std::shared_ptr<const exec_batch> exec_batch::capture(
    std::vector<std::shared_ptr<const page_batch>> batches) {
    if (batches.empty()) return nullptr;
    try {
        std::shared_ptr<exec_batch> out(new exec_batch);
        for (const auto& batch : batches) {
            if (!batch || batch->size().bytes >
                page_batch::max_bytes - out->bytes_ || batch->size().records >
                page_batch::max_records - out->records_) return nullptr;
            out->bytes_ += batch->size().bytes;
            out->records_ += batch->size().records;
        }
        out->batches_ = std::move(batches);
        return out;
    } catch (const std::bad_alloc&) { return nullptr; }
}
bool exec_batch::database(unsigned& out) const {
    if (has_moves() || batches_.empty()) return false;
    const auto first = batches_.front()->database();
    for (const auto& batch : batches_)
        if (batch->database() != first) return false;
    out = first; return true;
}
db::batch_status exec_batch::apply(db& destination) const {
    unsigned selected;
    if (!database(selected)) return db::batch_status::unsupported;
    page_sequence sequence;
    for (const auto& batch : batches_)
        if (!sequence.append(batch)) return db::batch_status::error;
    return sequence.apply(destination);
}
void exec_receiver::reset() {
    batch_.reset(); pages_.reset();
    expected_batches_ = expected_bytes_ = expected_records_ = 0;
    bytes_ = records_ = 0; failed_ = false;
}
exec_receiver::result exec_receiver::reject() {
    reset(); failed_ = true; return result::error;
}
bool exec_receiver::begin(const page_frame& frame) {
    if (frame.operation != page_operation::exec_begin || frame.database ||
        !frame.key.empty() || frame.id || frame.value.size() != 16)
        return false;
    const auto batches = page_wire::get32(frame.value.data());
    const auto records = page_wire::get32(frame.value.data() + 4);
    const auto bytes = page_wire::get64(frame.value.data() + 8);
    if (!batches || records > page_batch::max_records ||
        uint64_t(records) < 2 + uint64_t(batches) * 3 ||
        bytes > page_batch::max_bytes ||
        bytes < 56 + uint64_t(batches) * 76) return false;
    batch_.reset(new exec_batch);
    batch_->batches_.reserve(batches);
    expected_batches_ = batches; expected_records_ = records;
    expected_bytes_ = static_cast<size_t>(bytes);
    bytes_ = 36; records_ = 1;
    return true;
}
bool exec_receiver::nested_begin(const page_frame& frame) const {
    if (frame.operation != page_operation::begin) return true;
    if (pages_.active() || frame.value.size() != 16 ||
        batch_->batches_.size() >= expected_batches_) return false;
    const auto roots = page_wire::get32(frame.value.data());
    const auto pages = page_wire::get32(frame.value.data() + 4);
    const auto bytes = page_wire::get64(frame.value.data() + 8);
    return bytes <= expected_bytes_ - bytes_ - 20 &&
        2 + uint64_t(roots) + pages <= expected_records_ - records_ - 1;
}
exec_receiver::result exec_receiver::accept(page_frame frame,
    std::shared_ptr<const exec_batch>& out) {
    out.reset();
    if (failed_) return result::error;
    if (frame.protocol != 3) return reject();
    try {
        if (!batch_) return begin(frame) ? result::pending : reject();
        if (frame.operation == page_operation::exec_commit) {
            if (frame.database || !frame.key.empty() || frame.id ||
                !frame.value.empty() || pages_.active() ||
                batch_->batches_.size() != expected_batches_ ||
                records_ + 1 != expected_records_ ||
                bytes_ + 20 != expected_bytes_) return reject();
            batch_->bytes_ = expected_bytes_;
            batch_->records_ = expected_records_;
            out = batch_; reset(); return result::complete;
        }
        const bool page = frame.operation == page_operation::page_write ||
            frame.operation == page_operation::page_erase;
        const auto bytes = 20 + frame.key.size() + frame.value.size() +
            (page ? 8 : 0);
        if (records_ + 1 >= expected_records_ ||
            bytes > expected_bytes_ - bytes_ - 20 || !nested_begin(frame))
            return reject();
        std::shared_ptr<const page_batch> batch;
        const auto status = pages_.accept(std::move(frame), batch);
        if (status == result::error) return reject();
        if (status == result::complete) {
            if (batch_->batches_.size() >= expected_batches_) return reject();
            batch_->batches_.push_back(std::move(batch));
        }
        bytes_ += bytes; ++records_;
        return result::pending;
    } catch (const std::bad_alloc&) { return reject(); }
}

} // namespace pkv
