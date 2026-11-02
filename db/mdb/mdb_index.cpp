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
} // namespace pkv
