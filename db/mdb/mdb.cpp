//
// Created by shuxin ¡¡¡¡zheng on 2023/8/21.
//

#include "stdafx.h"
#include "mdb.h"

namespace pkv {

mdb::mdb() {
#if defined(USE_FOLLY)
    store_ = new folly::AtomicHashMap<std::string, std::string>(10000000);
#endif
}

mdb::~mdb() {
#if defined(USE_FOLLY)
    delete store_;
#endif
}

bool mdb::open(const char * /* path */) {
    return true;
}

bool mdb::set(const std::string &key, const std::string &value) {
    lock_.lock();
    store_[key] = value;
    lock_.unlock();
    return true;
}

bool mdb::get(const std::string &key, std::string &value) {
    lock_.lock();
    auto it = store_.find(key);
    if (it == store_.end()) {
        lock_.unlock();
        return false;
    }

    value = it->second;
    lock_.unlock();
    return true;
}

bool mdb::del(const std::string &key) {
    lock_.lock();
    auto it = store_.find(key);
    if (it == store_.end()) {
        lock_.unlock();
        return false;
    }

    store_.erase(it);
    lock_.unlock();
    return true;
}

bool mdb::scan(const std::string &key, std::vector<std::string> &keys,
       size_t max) {
    return false;
}

} // namespace pkv
