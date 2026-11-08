#include "expiry_index.h"
#include <functional>
#include <utility>

namespace pkv {
expiry_index::bucket& expiry_index::stripe(const std::string& key) {
    return buckets_[std::hash<std::string>()(key) % buckets_.size()];
}
expiry_index::hint& expiry_index::cached(bucket& value) {
    return hints_[static_cast<size_t>(&value - buckets_.data())];
}
void expiry_index::publish_first(bucket& value, bool exact) {
    const auto first = value.deadlines.empty() ? int64_t(-1) :
        value.deadlines.front()->second.deadline;
    auto& first_hint = cached(value).first;
    const auto previous = first_hint.load(std::memory_order_relaxed);
    if ((exact && previous != first) || previous < 0 || first < previous)
        first_hint.store(first, std::memory_order_relaxed);
}

void expiry_index::fix_heap(heap& deadlines, size_t position) {
    const auto before = [&](size_t left, size_t right) {
        return deadlines[left]->second.deadline < deadlines[right]->second.deadline;
    };
    const auto swap_positions = [&](size_t left, size_t right) {
        std::swap(deadlines[left], deadlines[right]);
        deadlines[left]->second.position = left;
        deadlines[right]->second.position = right;
    };
    while (position) {
        const auto parent = (position - 1) / 2;
        if (!before(position, parent)) break;
        swap_positions(position, parent);
        position = parent;
    }
    while (position < deadlines.size() / 2) {
        auto child = 2 * position + 1;
        if (child + 1 < deadlines.size() && before(child + 1, child)) ++child;
        if (!before(child, position)) break;
        swap_positions(position, child);
        position = child;
    }
}
bool expiry_index::set(const std::string& key, int64_t deadline) noexcept {
    if (deadline < 0) { erase(key); return !dirty(); }
    try {
        auto& bucket = stripe(key);
        std::lock_guard<std::mutex> guard(bucket.mutex);
        auto found = bucket.keys.find(key);
        if (found == bucket.keys.end()) {
            found = bucket.keys.emplace(key, entry()).first;
            try { bucket.deadlines.push_back(&*found); }
            catch (...) { bucket.keys.erase(found); throw; }
            found->second.position = bucket.deadlines.size() - 1;
        }
        if (found->second.deadline == deadline) return true;
        found->second.deadline = deadline;
        fix_heap(bucket.deadlines, found->second.position);
        publish_first(bucket, false);
        return true;
    } catch (...) { invalidate(); return false; }
}
void expiry_index::erase(const std::string& key) noexcept {
    try {
        auto& bucket = stripe(key);
        if (cached(bucket).first.load(std::memory_order_relaxed) < 0) return;
        std::lock_guard<std::mutex> guard(bucket.mutex);
        const auto found = bucket.keys.find(key);
        if (found == bucket.keys.end()) return;
        const auto position = found->second.position;
        auto* last = bucket.deadlines.back();
        bucket.deadlines.pop_back();
        if (position < bucket.deadlines.size()) {
            bucket.deadlines[position] = last;
            last->second.position = position;
            fix_heap(bucket.deadlines, position);
        }
        bucket.keys.erase(found);
        if (bucket.deadlines.empty()) heap().swap(bucket.deadlines);
    } catch (...) { invalidate(); }
}

bool expiry_index::collect_due(const heap& deadlines, size_t position,
    int64_t now, size_t limit, std::vector<std::string>& out) {
    if (position >= deadlines.size() ||
        deadlines[position]->second.deadline > now) return false;
    out.push_back(deadlines[position]->first);
    return out.size() == limit ||
        collect_due(deadlines, position * 2 + 1, now, limit, out) ||
        collect_due(deadlines, position * 2 + 2, now, limit, out);
}
bool expiry_index::due(int64_t now, size_t limit,
    std::vector<std::string>& out) {
    out.clear();
    if (!limit) return false;
    const auto first = cursor_.fetch_add(1, std::memory_order_relaxed);
    for (size_t i = 0; i < buckets_.size(); ++i) {
        auto& bucket = buckets_[(first + i) % buckets_.size()];
        const auto earliest =
            cached(bucket).first.load(std::memory_order_relaxed);
        if (earliest < 0 || earliest > now) continue;
        std::lock_guard<std::mutex> guard(bucket.mutex);
        publish_first(bucket, true);
        if (collect_due(bucket.deadlines, 0, now, limit, out)) return true;
    }
    return false;
}
size_t expiry_index::size() const {
    size_t count = 0;
    for (const auto& bucket : buckets_) {
        std::lock_guard<std::mutex> guard(bucket.mutex);
        count += bucket.keys.size();
    }
    return count;
}
} // namespace pkv
