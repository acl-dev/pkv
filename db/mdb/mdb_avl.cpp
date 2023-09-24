//
// Created by shuxin zheng on 2023/8/24.
//

#include "stdafx.h"
#include "mdb_avl.h"

namespace pkv {

static const char *key_names[] = { "key", "expire", nullptr };
static unsigned key_flags[] = { ACL_MDT_FLAG_NUL, ACL_MDT_FLAG_NUL };

mdb_avl::mdb_avl() {
    table_name_ = "test.table";
    mdb_ = acl_mdb_create("test.db", "avl");
    mdt_ = acl_mdb_tbl_create(mdb_, table_name_.c_str(), ACL_MDT_FLAG_NUL,
               10000, key_names, key_flags);
}

mdb_avl::~mdb_avl() {
    acl_mdt_free(mdt_);
    acl_mdb_free(mdb_);
}

bool mdb_avl::dbopen(const char *) {
    return true;
}

bool mdb_avl::dbset(const std::string &key, const std::string &value) {
    const char *key_values[3];
    key_values[0] = key.c_str();
    key_values[1] = "-1";
    key_values[2] = nullptr;

    (void) acl_mdb_add(mdb_, table_name_.c_str(),
               (void *) value.c_str(), value.size() + 1, key_names, key_values);
    return true;
}

bool mdb_avl::dbget(const std::string &key, std::string &value) {
    ACL_MDT_RES *res= acl_mdb_find(mdb_, table_name_.c_str(),
       key_names[0], key.c_str(), 0, 0);
    if (res == nullptr) {
        return false;
    }

    const void *val = acl_mdt_fetch_row(res);
    if (val == nullptr) {
        acl_mdt_res_free(res);
        return false;
    }

    value = (const char*) val;
    acl_mdt_res_free(res);
    return true;
}

bool mdb_avl::dbdel(const std::string &key) {
    int n = acl_mdb_del(mdb_, table_name_.c_str(), key_names[0], key.c_str(),
                        nullptr);
    return n > 0;
}

bool mdb_avl::dbscan(size_t idx, db_cursor& cursor, std::vector<std::string> &keys,
      size_t max) {
    return false;
}

} // namespace pkv
