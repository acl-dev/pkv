//
// Created by shuxin ¡¡¡¡zheng on 2023/8/21.
//

#include "stdafx.h"
#include "mdb.h"

namespace pkv {

bool mdb::open(const char * /* path */) {
    return true;
}

bool mdb::set(const std::string &key, const std::string &value) {
    store_[key] = value;
    return true;
}

bool mdb::get(const std::string &key, std::string &value) {
    auto it = store_.find(key);
    if (it == store_.end()) {
        return false;
    }

    value = it->second;
    return true;
}

bool mdb::del(const std::string &key) {
    auto it = store_.find(key);
    if (it == store_.end()) {
        return false;
    }

    store_.erase(it);
    return true;
}

bool mdb::scan(const std::string &key, std::vector<std::string> &keys,
       size_t max) {
    return false;
}

} // namespace pkv