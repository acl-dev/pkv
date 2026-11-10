#include "watch_registry.h"

namespace pkv {

watch_subscription::watch_subscription(std::shared_ptr<watch_registry> owner,
    std::string key, std::shared_ptr<epoch> state)
    : owner_(std::move(owner)), key_(std::move(key)),
      state_(std::move(state)) {}

watch_subscription::~watch_subscription() {
    if (armed_) owner_->release(key_, state_);
}

bool watch_subscription::valid(long long now) const {
    return !state_->dirty.load(std::memory_order_acquire) &&
        (state_->deadline < 0 || now < state_->deadline);
}

watch_registry::watch_bucket& watch_registry::stripe(const std::string& key) {
    return buckets_[std::hash<std::string>()(key) % buckets_.size()];
}

watch_token watch_registry::subscribe(const std::string& key, bool existed,
    long long deadline, long long now) {
    auto& bucket = stripe(key);
    std::lock_guard<std::mutex> guard(bucket.mutex);
    auto it = bucket.entries.find(key);
    const bool fresh = it == bucket.entries.end() ||
        it->second.state->dirty.load(std::memory_order_relaxed) ||
        (it->second.state->deadline >= 0 &&
            it->second.state->deadline <= now);
    auto state = fresh ? std::make_shared<watch_subscription::epoch>(
        existed, deadline) : it->second.state;
    // The unarmed destructor cannot reenter this mutex on allocation failure.
    watch_token token(new watch_subscription(shared_from_this(), key, state));
    if (it == bucket.entries.end())
        it = bucket.entries.emplace(key, entry()).first;
    if (fresh) {
        it->second.state = state;
        it->second.observers = 0;
    }
    ++it->second.observers;
    bucket.active.store(bucket.entries.size(), std::memory_order_release);
    token->armed_ = true;
    return token;
}

void watch_registry::release(const std::string& key,
    const std::shared_ptr<watch_subscription::epoch>& state) {
    auto& bucket = stripe(key);
    std::lock_guard<std::mutex> guard(bucket.mutex);
    auto it = bucket.entries.find(key);
    if (it == bucket.entries.end() || it->second.state != state) return;
    if (!--it->second.observers) bucket.entries.erase(it);
    bucket.active.store(bucket.entries.size(), std::memory_order_release);
}

void watch_registry::invalidate(const std::string& key, bool erasing) {
    auto& bucket = stripe(key);
    if (!bucket.active.load(std::memory_order_acquire)) return;
    std::lock_guard<std::mutex> guard(bucket.mutex);
    auto it = bucket.entries.find(key);
    if (it == bucket.entries.end() ||
        (erasing && !it->second.state->existed)) return;
    it->second.state->dirty.store(true, std::memory_order_release);
    bucket.entries.erase(it);
    bucket.active.store(bucket.entries.size(), std::memory_order_release);
}

} // namespace pkv
