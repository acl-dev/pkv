//
// Created by shuxin zheng on 2023/8/21.
//

#include "stdafx.h"
#include "mdb_tbb.h"

#if defined(HAS_TBB)

using Accessor = tbb::concurrent_hash_map<string_t, string_t>::accessor;

namespace pkv {

mdb_tbb::mdb_tbb() {
    for (size_t i = 0; i < 128; i++) {
        auto store = new atomic_map_t;
        stores_.push_back(store);
    }
}

mdb_tbb::~mdb_tbb() {
    for (auto store : stores_) {
        delete store;
    }
}

bool mdb_tbb::dbopen(const char * /* path */) {
    return true;
}

bool mdb_tbb::dbset(const std::string &key, const std::string &value) {
    unsigned n = acl_hash_crc32(key.c_str(), key.size()) % stores_.size();
    auto store = stores_[n];

    Accessor a; 
    store->insert(a, key);
    a->second = value;
    return true;
}

bool mdb_tbb::dbget(const std::string &key, std::string &value) {
    unsigned n = acl_hash_crc32(key.c_str(), key.size()) % stores_.size();
    auto store = stores_[n];

    Accessor a;
    const auto found = store->find(a, key);
    if (found) {
        value = a->second.c_str();
	return true;
    }
    return false;
}

bool mdb_tbb::dbdel(const std::string &key) {
    unsigned n = acl_hash_crc32(key.c_str(), key.size()) % stores_.size();
    auto store = stores_[n];

    return store->erase(key);
}

bool mdb_tbb::dbscan(size_t shard_id, db_cursor& cursor,
        std::vector<std::string> &keys, size_t max) {
    auto store = stores_[shard_id];
    Accessor a;
    for (auto it = store->begin(); it != store->end(); ++it) {
        if (keys.size() >= max) {
            break;
        }
        if (store->find(a, it->first)) {
            keys.push_back(a->first.c_str());
        }
    }
    return true;
}

} // namespace pkv

#endif // HAS_TBB
