#pragma once
#include "db.h"
#include <limits>

namespace pkv {
// Counts stored roots, including lazy-expired roots; pages never contribute.
inline int64_t root_count_delta(const std::vector<db::batch_entry>& roots) {
    int64_t delta = 0;
    for (const auto& root : roots) {
        if (root.action == db::update_action::write && !root.found) ++delta;
        else if (root.action == db::update_action::erase && root.found) --delta;
    }
    return delta;
}
inline bool add_key_count(uint64_t value, uint64_t& total) {
    if (value > std::numeric_limits<uint64_t>::max() - total) return false;
    total += value; return true;
}
} // namespace pkv
