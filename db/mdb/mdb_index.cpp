#include "stdafx.h"
#include "mdb.h"

namespace pkv {
void mdb::mark_key_shard(size_t shard, bool occupied) noexcept {
    if (shard >= tracked_key_shards) return;
    auto& word = occupied_key_shards_[shard / shard_word_bits];
    const auto bit = uint64_t{1} << (shard % shard_word_bits);
    if (occupied) word.fetch_or(bit, std::memory_order_relaxed);
    else word.fetch_and(~bit, std::memory_order_relaxed);
}

bool mdb::add_root(size_t shard, root_map& bucket, const std::string& key,
    const std::string& value) {
    auto& keys = stores_[shard]->keys;
    keys.prepare();
    try {
        const auto inserted = bucket.emplace(key, root_record(value));
        if (!inserted.second) { keys.cancel(); return false; }
        keys.publish(&*inserted.first);
        const auto size = keys.size();
        if (size == 1) mark_key_shard(shard, true);
        auto bound = sample_shard_bound_.load(std::memory_order_relaxed);
        while (bound < size && !sample_shard_bound_.compare_exchange_weak(
            bound, size, std::memory_order_relaxed)) {}
        root_count_.fetch_add(1, std::memory_order_relaxed);
        return true;
    } catch (const std::bad_alloc&) { keys.cancel(); throw; }
}
void mdb::erase_root(size_t shard, root_map& bucket, root_map::iterator node) {
    auto& keys = stores_[shard]->keys;
    keys.erase(&*node);
    if (!keys.size()) mark_key_shard(shard, false);
    bucket.erase(node);
    root_count_.fetch_sub(1, std::memory_order_relaxed);
}
bool mdb::erase_root(const location& position, const std::string& key) {
    auto& shard = *stores_[position.shard];
    auto& bucket = shard.buckets[position.bucket];
    if (!bucket) return false;
    const auto node = bucket->find(key);
    if (node == bucket->end()) return false;
    erase_root(position.shard, *bucket, node);
    if (bucket->empty()) bucket.reset();
    return true;
}
} // namespace pkv
