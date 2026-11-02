#pragma once
#include "set_sampling.h"
#include <cstdint>
#include <unordered_map>

namespace pkv {

// Caller supplies a nonempty population and requests at most size distinct
// ranks. Sparse Fisher-Yates needs O(sample size), not O(population), memory.
class rank_sampler {
public:
    rank_sampler(uint64_t size, bool repeat)
    : size_(size), repeat_(repeat), random_(set_sampling_engine()) {}

    uint64_t next() {
        const auto position = std::uniform_int_distribution<uint64_t>(
            repeat_ ? 0 : selected_, size_ - 1)(random_);
        const auto saved = swapped_.find(position);
        const auto rank = saved == swapped_.end() ? position : saved->second;
        if (!repeat_) {
            const auto current = swapped_.find(selected_);
            const auto replacement = current == swapped_.end() ? selected_ :
                current->second;
            swapped_[position] = replacement;
            swapped_.erase(selected_++);
        }
        return rank;
    }
private:
    const uint64_t size_;
    const bool repeat_;
    uint64_t selected_ = 0;
    std::mt19937_64& random_;
    std::unordered_map<uint64_t, uint64_t> swapped_;
};

} // namespace pkv
