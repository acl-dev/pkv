//
// Created by shuxin ¡¡¡¡zheng on 2023/8/21.
//

#include "stdafx.h"
#include "mdb_htable_cursor.h"
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

    for (size_t i = 0; i < 256; i++) {
        auto store = acl_htable_create3(100000, flags, slice_);
        acl_htable_ctl(store, ACL_HTABLE_CTL_HASH_FN, acl_hash_func5,
            ACL_HTABLE_CTL_END);
        dbs_.push_back(store);
    }
}

mdb_htable::~mdb_htable() {
    for (auto dbp : dbs_) {
        acl_htable_free(dbp, slice_ ? nullptr : free);
    }

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

    unsigned n = acl_hash_crc32(key.c_str(), key.size()) % dbs_.size();
    auto store = dbs_[n];

    void* old = nullptr;
    ACL_HTABLE_INFO* info = acl_htable_enter_r2(store, key.c_str(), buf, &old);
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
    unsigned n = acl_hash_crc32(key.c_str(), key.size()) % dbs_.size();
    auto store = dbs_[n];

    void* data = acl_htable_find(store, key.c_str());
    if (data == nullptr) {
        return false;
    }
    value = (char*) data;
    return true;
}

bool mdb_htable::del(const std::string &key) {
    unsigned n = acl_hash_crc32(key.c_str(), key.size()) % dbs_.size();
    auto store = dbs_[n];

    int ret = acl_htable_delete(store, key.c_str(), acl_myfree_fn);
    return ret == 0;
}

db_cursor *mdb_htable::create_cursor() {
    return new mdb_htable_cursor(*this);
}

bool mdb_htable::scan(db_cursor& cursor, std::vector<std::string> &keys, size_t max) {
    mdb_htable_cursor& cur = (mdb_htable_cursor&) cursor;
    keys.clear();
    while (true) {
        size_t idx = cursor.get_db();
        if (idx >= dbs_.size()) {
            return true;
        }

        auto dbp = dbs_[idx];
        if (!scan(*dbp, cur, keys, max)) {
            return false;
        }

        if (keys.size() >= max) {
            return true;
        }

        // If not got the needed keys, we should reset the cursor for next db.
        cur.idx_ = 0;
        cursor.next_db();
    }
}

bool mdb_htable::scan(ACL_HTABLE& db, mdb_htable_cursor& cursor,
      std::vector<std::string>& keys, size_t max) {

    acl_htable_lock_read(&db);

    while (keys.size() < max) {
        const ACL_HTABLE_INFO *info;

        if (cursor.idx_ == 0) {
            info = acl_htable_iter_head(&db, &cursor.iter_);
        } else {
            info = acl_htable_iter_next(&cursor.iter_);
        }

        if (info == nullptr) {
            break;
        }

        cursor.idx_++;
        keys.emplace_back(info->key.key);
    }

    acl_htable_unlock(&db);
    return true;
}

} // namespace pkv
