#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include <unordered_map>

namespace pkv {
// One key copy and one deadline node per expiring owner, without stale heaps.
struct expiry_totals { uint64_t count = 0, average = 0; };
class expiry_index {
public:
    bool set(const std::string& key, int64_t deadline) noexcept;
    void erase(const std::string& key) noexcept;
    bool due(int64_t now, size_t limit, std::vector<std::string>& out);
    bool dirty() const { return dirty_.load(std::memory_order_acquire); }
    void invalidate() { dirty_.store(true, std::memory_order_release); }
    size_t size() const;
    expiry_totals snapshot(int64_t now) const;
private:
    using times = std::multimap<int64_t, const std::string*>;
    struct entry {
        int64_t deadline = -1;
        times::iterator position;
    };
    struct hint {
        // Hint only: maps always need mutex. Same-owner DB mutations hold
        // the key lock; polls may defer concurrent updates to the next pass.
        // Atomic coherence suffices; this does not publish map contents.
        // A nonnegative hint is a lower bound; deletes may leave it stale.
        // -1 proves empty; all stored deadlines are nonnegative.
        std::atomic<int64_t> first{-1};
    };
    struct bucket {
        mutable std::mutex mutex;
        std::unordered_map<std::string, entry> owners;
        times deadlines;
    };
    std::array<bucket, 64> buckets_;
    std::array<hint, 64> hints_;
    std::atomic<bool> dirty_{false};
    std::atomic<size_t> cursor_{0};
    bucket& stripe(const std::string& key);
    hint& cached(bucket& value);
    void publish_first(bucket& value, bool exact);
};
} // namespace pkv
