#include "expiry_index.h"
#include <functional>
#include <stdexcept>
#include <utility>

namespace pkv {
namespace {
constexpr size_t initial_key_slots = 4;
}

size_t expiry_index::expiry_bucket::find_slot(size_t hash, const std::string& key) const {
    if (keys.empty()) return 0;
    const auto mask = keys.size() - 1;
    auto slot = home_slot(hash, mask);
    while (const auto* item = keys[slot]) {
        if (item->hash == hash && item->key == key) break;
        slot = (slot + 1) & mask;
    }
    return slot;
}

void expiry_index::expiry_bucket::rehash_keys(size_t capacity) {
    std::vector<entry*> next(capacity);
    const auto mask = next.size() - 1;
    for (const auto& item : deadlines) {
        auto slot = home_slot(item->hash, mask);
        while (next[slot]) slot = (slot + 1) & mask;
        next[slot] = item.get();
    }
    keys.swap(next);
}

void expiry_index::expiry_bucket::erase_slot(size_t slot) noexcept {
    const auto mask = keys.size() - 1;
    keys[slot] = nullptr;
    for (auto next = (slot + 1) & mask; keys[next]; next = (next + 1) & mask) {
        const auto home = home_slot(keys[next]->hash, mask);
        // Move an entry only if its probe crosses the vacant slot.
        if (((next - home) & mask) < ((next - slot) & mask)) continue;
        keys[slot] = keys[next];
        keys[next] = nullptr;
        slot = next;
    }
}
expiry_index::hint& expiry_index::cached(expiry_bucket& value) {
    return hints_[static_cast<size_t>(&value - buckets_.data())];
}
void expiry_index::publish_first(expiry_bucket& value, bool exact) {
    const auto first = value.deadlines.empty() ? int64_t(-1) :
        value.deadlines.front()->deadline;
    auto& first_hint = cached(value).first;
    const auto previous = first_hint.load(std::memory_order_relaxed);
    if ((exact && previous != first) || previous < 0 || first < previous)
        first_hint.store(first, std::memory_order_relaxed);
}

void expiry_index::fix_heap(heap& deadlines, size_t position) {
    const auto before = [&](size_t left, size_t right) {
        return deadlines[left]->deadline < deadlines[right]->deadline;
    };
    const auto swap_positions = [&](size_t left, size_t right) {
        std::swap(deadlines[left], deadlines[right]);
        deadlines[left]->position = left;
        deadlines[right]->position = right;
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
        const auto hash = std::hash<std::string>()(key);
        auto& bucket = buckets_[hash % stripe_count];
        std::lock_guard<std::mutex> guard(bucket.mutex);
        auto slot = bucket.find_slot(hash, key);
        auto* item = bucket.keys.empty() ? nullptr : bucket.keys[slot];
        if (item) {
            if (item->deadline == deadline) return true;
#if defined(__SIZEOF_INT128__)
            bucket.deadline_total -= item->deadline;
#endif
            item->deadline = deadline;
        } else {
            if (bucket.deadlines.size() >= bucket.keys.size() - bucket.keys.size() / 4) {
                if (bucket.keys.size() > bucket.keys.max_size() / 2)
                    throw std::length_error("expiry slots");
                bucket.rehash_keys(bucket.keys.empty() ? initial_key_slots :
                    bucket.keys.size() * 2);
                slot = bucket.find_slot(hash, key);
            }
            std::unique_ptr<entry> created(new entry(key, deadline, hash,
                bucket.deadlines.size()));
            item = created.get();
            bucket.deadlines.push_back(std::move(created));
            bucket.keys[slot] = item;
        }
#if defined(__SIZEOF_INT128__)
        bucket.deadline_total += deadline;
#endif
        fix_heap(bucket.deadlines, item->position);
        publish_first(bucket, false);
        return true;
    } catch (...) { invalidate(); return false; }
}
void expiry_index::erase(const std::string& key) noexcept {
    try {
        const auto hash = std::hash<std::string>()(key);
        auto& bucket = buckets_[hash % stripe_count];
        if (cached(bucket).first.load(std::memory_order_relaxed) < 0) return;
        std::lock_guard<std::mutex> guard(bucket.mutex);
        if (bucket.keys.empty()) return;
        const auto slot = bucket.find_slot(hash, key);
        const auto* item = bucket.keys[slot];
        if (!item) return;
        const auto position = item->position;
#if defined(__SIZEOF_INT128__)
        bucket.deadline_total -= item->deadline;
#endif
        bucket.erase_slot(slot);
        if (position + 1 != bucket.deadlines.size()) {
            bucket.deadlines[position] = std::move(bucket.deadlines.back());
            bucket.deadlines[position]->position = position;
        }
        bucket.deadlines.pop_back();
        if (position < bucket.deadlines.size())
            fix_heap(bucket.deadlines, position);
        if (bucket.deadlines.empty()) {
            heap().swap(bucket.deadlines);
            std::vector<entry*>().swap(bucket.keys);
            publish_first(bucket, true);
        } else if (bucket.keys.size() > initial_key_slots &&
            bucket.deadlines.size() <= bucket.keys.size() / 8) {
            try { bucket.rehash_keys(bucket.keys.size() / 2); }
            catch (const std::bad_alloc&) {} // Reclamation is optional.
        }
    } catch (...) { invalidate(); }
}

bool expiry_index::collect_due(const heap& deadlines, size_t position,
    int64_t now, size_t limit, std::vector<std::string>& out) {
    if (position >= deadlines.size() ||
        deadlines[position]->deadline > now) return false;
    out.push_back(deadlines[position]->key);
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
        count += bucket.deadlines.size();
    }
    return count;
}
} // namespace pkv
