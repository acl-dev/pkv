#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace pkv {
// The deadline heap owns records. The hash slots borrow their addresses.
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
    static constexpr size_t stripe_count = 64;
    struct entry {
        std::string key;
        int64_t deadline;
        size_t hash;
        size_t position;
        entry(const std::string& name, int64_t when, size_t hashed, size_t index)
            : key(name), deadline(when), hash(hashed), position(index) {}
    };
    using heap = std::vector<std::unique_ptr<entry>>;
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
        std::vector<entry*> keys;
        heap deadlines;
#if defined(__SIZEOF_INT128__)
        __uint128_t deadline_total = 0;
#endif
        size_t find_slot(size_t hash, const std::string& key) const;
        void rehash_keys(size_t capacity);
        void erase_slot(size_t slot) noexcept;
    };
    std::array<bucket, stripe_count> buckets_;
    std::array<hint, stripe_count> hints_;
    std::atomic<bool> dirty_{false};
    std::atomic<size_t> cursor_{0};
    static size_t home_slot(size_t hash, size_t mask) noexcept {
        return (hash / stripe_count) & mask;
    }
    hint& cached(bucket& value);
    void publish_first(bucket& value, bool exact);
    static void fix_heap(heap& deadlines, size_t position);
    static bool collect_due(const heap& deadlines, size_t position,
        int64_t now, size_t limit, std::vector<std::string>& out);
};
} // namespace pkv
