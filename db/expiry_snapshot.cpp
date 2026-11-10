#include "expiry_index.h"

namespace pkv {
expiry_totals expiry_index::snapshot(int64_t now) const {
#if defined(__SIZEOF_INT128__)
    {
        expiry_totals exact;
        __uint128_t deadlines = 0;
        bool expired = false;
        for (const auto& bucket : buckets_) {
            std::lock_guard<std::mutex> lock(bucket.mutex);
            if (bucket.deadlines.empty()) continue;
            if (bucket.deadlines.front()->deadline < now) {
                expired = true;
                break;
            }
            exact.count += bucket.deadlines.size();
            deadlines += bucket.deadline_total;
        }
        if (!expired) {
            // Subtracting an integer commutes with taking the floor.
            if (exact.count) exact.average = static_cast<uint64_t>(
                deadlines / exact.count) - static_cast<uint64_t>(now);
            return exact;
        }
    }
#endif
    expiry_totals result;
#if defined(__SIZEOF_INT128__)
    // Up to 1024 samples, each potentially as large as UINT64_MAX.
    __uint128_t total = 0;
#else
    std::array<uint64_t, 1024> values;
#endif
    size_t samples = 0;
    for (const auto& bucket : buckets_) {
        std::lock_guard<std::mutex> lock(bucket.mutex);
        result.count += bucket.deadlines.size();
        size_t count = 0;
        for (const auto* item : bucket.keys) {
            if (!item) continue;
            if (count++ == 16) break;
            const auto deadline = item->deadline;
            const auto ttl = deadline > now ? static_cast<uint64_t>(deadline) -
                static_cast<uint64_t>(now) : 0;
#if defined(__SIZEOF_INT128__)
            total += ttl;
#else
            values[samples] = ttl;
#endif
            ++samples;
        }
    }
#if defined(__SIZEOF_INT128__)
    if (samples) result.average = static_cast<uint64_t>(total / samples);
#else
    uint64_t remainder = 0;
    for (size_t i = 0; i < samples; ++i) {
        result.average += values[i] / samples;
        const auto part = values[i] % samples;
        if (remainder >= samples - part) {
            ++result.average; remainder -= samples - part;
        } else remainder += part;
    }
#endif
    return result;
}
} // namespace pkv
