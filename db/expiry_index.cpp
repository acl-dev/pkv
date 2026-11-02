#include "expiry_index.h"
#include <functional>

namespace pkv {
expiry_index::bucket& expiry_index::stripe(const std::string& key) {
    return buckets_[std::hash<std::string>()(key) % buckets_.size()];
}
expiry_index::hint& expiry_index::cached(bucket& value) {
    return hints_[static_cast<size_t>(&value - buckets_.data())];
}
void expiry_index::publish_first(bucket& value, bool exact) {
    const auto first = value.deadlines.empty() ? int64_t(-1) :
        value.deadlines.begin()->first;
    auto& first_hint = cached(value).first;
    const auto previous = first_hint.load(std::memory_order_relaxed);
    if ((exact && previous != first) || previous < 0 || first < previous)
        first_hint.store(first, std::memory_order_relaxed);
}
bool expiry_index::set(const std::string& key, int64_t deadline) noexcept {
    if (deadline < 0) { erase(key); return !dirty(); }
    try {
        auto& bucket = stripe(key);
        std::lock_guard<std::mutex> guard(bucket.mutex);
        auto found = bucket.owners.find(key);
        const bool added = found == bucket.owners.end();
        if (!added && found->second.deadline == deadline) return true;
        if (added) found = bucket.owners.emplace(key, entry()).first;
        times::iterator position;
        try { position = bucket.deadlines.emplace(deadline, &found->first); }
        catch (...) {
            if (added) bucket.owners.erase(found);
            throw;
        }
        if (!added) bucket.deadlines.erase(found->second.position);
        found->second.deadline = deadline;
        found->second.position = position;
        publish_first(bucket, false);
        return true;
    } catch (...) { invalidate(); return false; }
}
void expiry_index::erase(const std::string& key) noexcept {
    try {
        auto& bucket = stripe(key);
        if (cached(bucket).first.load(std::memory_order_relaxed) < 0) return;
        std::lock_guard<std::mutex> guard(bucket.mutex);
        const auto found = bucket.owners.find(key);
        if (found == bucket.owners.end()) return;
        bucket.deadlines.erase(found->second.position);
        bucket.owners.erase(found);
    } catch (...) { invalidate(); }
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
        for (const auto& deadline : bucket.deadlines) {
            if (deadline.first > now) break;
            out.push_back(*deadline.second);
            if (out.size() == limit) return true;
        }
    }
    return false;
}
size_t expiry_index::size() const {
    size_t count = 0;
    for (const auto& bucket : buckets_) {
        std::lock_guard<std::mutex> guard(bucket.mutex);
        count += bucket.owners.size();
    }
    return count;
}
} // namespace pkv
