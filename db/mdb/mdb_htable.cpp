//
// Created by shuxin ¡¡¡¡zheng on 2023/8/21.
//

#include "stdafx.h"
#include "mdb_htable.h"

namespace pkv {

mdb_htable::mdb_htable() {
# ifdef USE_MEM_SLICE
    slice_ = acl_slice_pool_create(4, 10000,
            ACL_SLICE_FLAG_GC2 | ACL_SLICE_FLAG_RTGC_OFF);
# else
    slice_ = nullptr;
# endif

    unsigned flags = ACL_HTABLE_FLAG_USE_LOCK;
    store_ = acl_htable_create3(10000000, flags, slice_);
    acl_htable_ctl(store_, ACL_HTABLE_CTL_HASH_FN, acl_hash_func5,
            ACL_HTABLE_CTL_END);
}

mdb_htable::~mdb_htable() {
    acl_htable_free(store_, slice_ ? nullptr : free);
    if (slice_) {
        acl_slice_pool_destroy(slice_);
    }
}

bool mdb_htable::open(const char * /* path */) {
    return true;
}

bool mdb_htable::set(const std::string &key, const std::string &value) {
    char* buf;

    if (slice_) {
        buf = (char*) acl_slice_pool_memdup(__FILE__, __LINE__,
                 slice_, value.c_str(), value.size() + 1);
    } else {
        buf = (char*) malloc(value.size() + 1);
        memcpy(buf, value.c_str(), value.size());
    }

    buf[value.size()] = 0;

    void* old = nullptr;
    ACL_HTABLE_INFO* info = acl_htable_enter_r2(store_, key.c_str(), buf, &old);
    if (old) {
        if (slice_) {
            acl_slice_pool_free(__FILE__, __LINE__, old);
        } else {
            free(old);
        }
    }

    if (info == nullptr) {
        return false;
    }
    return true;
}

bool mdb_htable::get(const std::string &key, std::string &value) {
    void* data = acl_htable_find(store_, key.c_str());
    if (data == nullptr) {
        return false;
    }
    value = (char*) data;
    return true;
}

bool mdb_htable::del(const std::string &key) {
    int ret = acl_htable_delete(store_, key.c_str(), acl_myfree_fn);
    return ret == 0;
}

bool mdb_htable::scan(const std::string &key, std::vector<std::string> &keys,
       size_t max) {
    return false;
}

} // namespace pkv
