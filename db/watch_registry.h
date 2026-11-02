#pragma once
#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace pkv {

class watch_registry;
class watch_subscription {
public:
    ~watch_subscription();
    bool valid(long long now) const;
private:
    friend class watch_registry;
    struct epoch {
        std::atomic<bool> dirty{false};
        bool existed;
        long long deadline;
        epoch(bool present, long long expiry)
            : existed(present), deadline(expiry) {}
    };
    watch_subscription(std::shared_ptr<watch_registry> owner,
        std::string key, std::shared_ptr<epoch> state);
    std::shared_ptr<watch_registry> owner_;
    std::string key_;
    std::shared_ptr<epoch> state_;
    bool armed_ = false;
};
using watch_token = std::shared_ptr<watch_subscription>;

// Key locks serialize subscribe with commit. These short registry locks never
// yield, access storage, or call observers. Unwatched stripes avoid locking.
class watch_registry : public std::enable_shared_from_this<watch_registry> {
public:
    watch_token subscribe(const std::string& key, bool existed,
        long long deadline, long long now);
    void invalidate(const std::string& key, bool erasing = false);
private:
    friend class watch_subscription;
    struct entry {
        std::shared_ptr<watch_subscription::epoch> state;
        size_t observers = 0;
    };
    struct bucket {
        std::mutex mutex;
        std::atomic<size_t> active{0};
        std::unordered_map<std::string, entry> entries;
    };
    std::array<bucket, 256> buckets_;
    bucket& stripe(const std::string& key);
    void release(const std::string& key,
        const std::shared_ptr<watch_subscription::epoch>& state);
};

} // namespace pkv
