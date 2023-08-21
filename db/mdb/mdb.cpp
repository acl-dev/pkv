//
// Created by shuxin ¡¡¡¡zheng on 2023/8/21.
//

#include "stdafx.h"
#include "mdb.h"

namespace pkv {

mdb::mdb() {
#ifdef USE_HTABLE
# ifdef USE_MEM_SLICE
    slice_ = acl_slice_pool_create(4, 10000,
            ACL_SLICE_FLAG_GC2 | ACL_SLICE_FLAG_RTGC_OFF);
# else
    slice_ = NULL;
# endif
    store_ = acl_htable_create3(20000000, 0, slice_);
    acl_htable_ctl(store_, ACL_HTABLE_CTL_HASH_FN, acl_hash_func5,
            ACL_HTABLE_CTL_END);
#endif
}

mdb::~mdb() {
#ifdef USE_HTABLE
    acl_htable_free(store_, slice_ ? nullptr : free);
    if (slice_) {
        acl_slice_pool_destroy(slice_);
    }
#endif
}

bool mdb::open(const char * /* path */) {
    return true;
}

bool mdb::set(const std::string &key, const std::string &value) {
#ifdef USE_HTABLE
    char* buf;

    if (slice_) {
        buf = (char*) acl_slice_pool_memdup(__FILE__, __LINE__,
                 slice_, value.c_str(), value.size() + 1);
    } else {
        buf = (char*) malloc(value.size() + 1);
        memcpy(buf, value.c_str(), value.size());
    }

    buf[value.size()] = 0;

    ACL_HTABLE_INFO* info = acl_htable_enter(store_, key.c_str(), buf);
    if (store_->status == ACL_HTABLE_STAT_DUPLEX_KEY) {
        if (slice_) {
            acl_slice_pool_free(__FILE__, __LINE__, info->value);
        } else {
            free(info->value);
        }
        info->value = buf;
    }
#else
    store_[key] = value;
#endif
    return true;
}

bool mdb::get(const std::string &key, std::string &value) {
#ifdef USE_HTABLE
    void* data = acl_htable_find(store_, key.c_str());
    if (data == nullptr) {
        return false;
    }
    value = (char*) data;
#else
    auto it = store_.find(key);
    if (it == store_.end()) {
        return false;
    }

    value = it->second;
#endif
    return true;
}

bool mdb::del(const std::string &key) {
#ifdef USE_HTABLE
    int ret = acl_htable_delete(store_, key.c_str(), acl_myfree_fn);
    return ret == 0 ? true : false;
#else
    auto it = store_.find(key);
    if (it == store_.end()) {
        return false;
    }

    store_.erase(it);
    return true;
#endif
}

bool mdb::scan(const std::string &key, std::vector<std::string> &keys,
       size_t max) {
    return false;
}

} // namespace pkv
