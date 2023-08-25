//
// Created by shuxin ¡¡¡¡zheng on 2023/8/21.
//

#include "stdafx.h"
#include "mdb_tbb.h"

#if defined(HAS_TBB)

typedef tbb::concurrent_hash_map<string_t, string_t>::accessor Accessor;

namespace pkv {

mdb_tbb::mdb_tbb() {}

mdb_tbb::~mdb_tbb() {}

bool mdb_tbb::open(const char * /* path */) {
    return true;
}

bool mdb_tbb::set(const std::string &key, const std::string &value) {
    Accessor a; 
    store_.insert(a, key);
    a->second = value;
    return true;
}

bool mdb_tbb::get(const std::string &key, std::string &value) {
    Accessor a;
    const auto found = store_.find(a, key);
    if (found) {
        value = a->second.c_str();
	return true;
    }
    return false;
}

bool mdb_tbb::del(const std::string &key) {
    return store_.erase(key);
}

bool mdb_tbb::scan(const std::string &key, std::vector<std::string> &keys,
       size_t max) {
    return false;
}

} // namespace pkv

#endif // HAS_TBB
