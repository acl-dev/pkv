#pragma once
#include "exec_batch.h"

namespace pkv {
// Network assembly only. Admission/native state are acquired after COMPLETE.
class ordered_receiver {
public:
    using result = page_receiver::result;
    result accept(page_frame frame, std::shared_ptr<const exec_batch>& out);
    void reset();
    bool active() const { return active_; }
private:
    std::vector<exec_entry> entries_;
    page_receiver pages_;
    size_t expected_entries_ = 0, expected_bytes_ = 0, expected_records_ = 0;
    size_t bytes_ = 0, records_ = 0;
    bool active_ = false, failed_ = false;
    bool begin(const page_frame& frame);
    bool nested_begin(const page_frame& frame) const;
    result reject();
};
} // namespace pkv
