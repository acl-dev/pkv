#pragma once
#include "set_sampling.h"
#include <algorithm>
#include <array>
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

// Draw uniformly from the remaining ranks. Small samples can translate the
// draw by skipping the already selected ranks in a fixed sorted array.
template<class Select>
bool visit_sampled_ranks(uint64_t size, uint64_t count, bool repeat,
    const Select& select) {
    if (repeat) {
        auto& random = set_sampling_engine();
        std::uniform_int_distribution<uint64_t> draw(0, size - 1);
        for (uint64_t i = 0; i < count; ++i)
            if (!select(draw(random))) return false;
        return true;
    }
    std::array<uint64_t, 8> selected;
    if (count > selected.size()) {
        rank_sampler ranks(size, false);
        for (uint64_t i = 0; i < count; ++i)
            if (!select(ranks.next())) return false;
        return true;
    }
    auto& random = set_sampling_engine();
    for (size_t i = 0; i < count; ++i) {
        auto rank = std::uniform_int_distribution<uint64_t>(0, size - i - 1)(random);
        auto position = selected.begin();
        const auto end = position + i;
        while (position != end && *position <= rank) {
            ++rank;
            ++position;
        }
        std::move_backward(position, end, end + 1);
        *position = rank;
        if (!select(rank)) return false;
    }
    return true;
}

} // namespace pkv
