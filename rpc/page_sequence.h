#pragma once
#include "page_batch.h"

namespace pkv {

// One database's ordered mutations, staged together and committed once. This
// is a storage primitive, not yet an EXEC wire envelope or cross-DB commit.
class page_sequence {
public:
    bool append(std::shared_ptr<const page_batch> batch);
    db::batch_status apply(db& destination) const;
    size_t bytes() const { return bytes_; }
    size_t records() const { return records_; }
    size_t size() const { return batches_.size(); }
    unsigned database() const { return database_; }
    void clear();
private:
    unsigned database_ = 0;
    size_t bytes_ = 0, records_ = 0;
    std::vector<std::shared_ptr<const page_batch>> batches_;
};

} // namespace pkv
