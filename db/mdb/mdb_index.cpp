#include "stdafx.h"
#include "mdb.h"

namespace pkv {
bool mdb::add_root(store& shard, root_map& bucket, const std::string& key,
    const std::string& value) {
    shard.keys.prepare();
    try {
        const auto inserted = bucket.emplace(key, root_record(value));
        if (!inserted.second) { shard.keys.cancel(); return false; }
        shard.keys.publish(&*inserted.first);
        return true;
    } catch (const std::bad_alloc&) { shard.keys.cancel(); throw; }
}
void mdb::erase_root(store& shard, root_map& bucket, root_map::iterator node) {
    shard.keys.erase(&*node);
    bucket.erase(node);
}
bool mdb::erase_root(const location& position, const std::string& key) {
    auto& shard = *stores_[position.shard];
    auto& bucket = shard.buckets[position.bucket];
    if (!bucket) return false;
    const auto node = bucket->find(key);
    if (node == bucket->end()) return false;
    erase_root(shard, *bucket, node);
    if (bucket->empty()) bucket.reset();
    return true;
}
} // namespace pkv
