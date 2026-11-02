#include "page_sequence.h"
#include <new>

namespace pkv {

bool page_sequence::append(std::shared_ptr<const page_batch> batch) {
    if (!batch || (!batches_.empty() && database_ != batch->database()) ||
        batch->size().bytes > page_batch::max_bytes - bytes_ ||
        batch->size().records > page_batch::max_records - records_)
        return false;
    try { batches_.push_back(batch); }
    catch (const std::bad_alloc&) { return false; }
    database_ = batch->database();
    bytes_ += batch->size().bytes;
    records_ += batch->size().records;
    return true;
}

db::batch_status page_sequence::apply(db& destination) const {
    try {
        std::vector<page_update> updates;
        updates.reserve(batches_.size());
        for (const auto& batch : batches_)
            updates.emplace_back(batch->roots(), batch->pages());
        return destination.replica_apply_pages(updates);
    } catch (const std::bad_alloc&) { return db::batch_status::error; }
}
void page_sequence::clear() {
    std::vector<std::shared_ptr<const page_batch>>().swap(batches_);
    bytes_ = records_ = 0; database_ = 0;
}

} // namespace pkv
