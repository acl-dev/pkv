#include "expiry_index.h"

namespace pkv {
expiry_totals expiry_index::snapshot(int64_t now) const {
    expiry_totals result;
    std::array<uint64_t, 1024> values;
    size_t samples = 0;
    for (const auto& bucket : buckets_) {
        std::lock_guard<std::mutex> lock(bucket.mutex);
        result.count += bucket.owners.size();
        size_t count = 0;
        for (const auto& item : bucket.owners) {
            if (count++ == 16) break;
            const auto deadline = item.second.deadline;
            values[samples++] = deadline > now ?
                static_cast<uint64_t>(deadline) -
                static_cast<uint64_t>(now) : 0;
        }
    }
    uint64_t remainder = 0;
    for (size_t i = 0; i < samples; ++i) {
        result.average += values[i] / samples;
        const auto part = values[i] % samples;
        if (remainder >= samples - part) {
            ++result.average; remainder -= samples - part;
        } else remainder += part;
    }
    return result;
}
} // namespace pkv
