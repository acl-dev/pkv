//
// Created by shuxin zheng on 2023/8/21.
//

#include "stdafx.h"
#include "mdb.h"

namespace pkv {

mdb::mdb() {
    for (size_t i = 0; i < 1024; i++) {
        auto lock = new lock_t;
	locks_.push_back(lock);

	auto store = new map_t;
	stores_.push_back(store);
    }
}

mdb::~mdb() {
    for (size_t i = 0; i < stores_.size(); i++) {
        delete locks_[i];
        delete stores_[i];
    }
}

bool mdb::dbopen(const char * /* path */) {
    return true;
}

bool mdb::dbset(const std::string &key, const std::string &value) {
    unsigned n = acl_hash_crc32(key.c_str(), key.size()) % stores_.size();
    auto store = stores_[n];

    locks_[n]->lock();

    (*store)[key] = value;

    locks_[n]->unlock();
    return true;
}

bool mdb::dbget(const std::string &key, std::string &value) {
    unsigned n = acl_hash_crc32(key.c_str(), key.size()) % stores_.size();
    auto store = stores_[n];

    locks_[n]->lock();

    auto it = store->find(key);
    if (it == store->end()) {
    	locks_[n]->unlock();
        return false;
    }

    value = it->second.c_str();
    locks_[n]->unlock();
    return true;
}

bool mdb::dbdel(const std::string &key) {
    unsigned n = acl_hash_crc32(key.c_str(), key.size()) % stores_.size();
    auto store = stores_[n];

    locks_[n]->lock();

    auto it = store->find(key);
    if (it == store->end()) {
    	locks_[n]->unlock();
        return false;
    }

    store->erase(it);
    locks_[n]->unlock();
    return true;
}

bool mdb::dbscan(size_t idx, db_cursor& cursor, std::vector<std::string> &keys,
      size_t max) {
    return false;
}

} // namespace pkv
