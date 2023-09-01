//
// Created by shuxin ¡¡¡¡zheng on 2023/7/27.
//

#include "stdafx.h"

#ifdef HAS_WT

#include "wt.h"
#include "wdb.h"

namespace pkv {

wdb::wdb(size_t cache_max) : cache_max_(cache_max) {}

wdb::~wdb() {
    for (auto it : dbs_) {
        delete it;
    }
}

bool wdb::open(const char *paths) {
    acl::string buf(paths);
    auto& tokens = buf.split2(";, \t");
    for (auto token : tokens) {
        std::string path = token;
        path += "/wdb";
        if (!open_one(path)) {
            return false;
        }
    }

    return true;
}

bool wdb::open_one(const std::string& path) {
    if (acl_make_dirs(path.c_str(), 0755) == -1) {
        logger_error("create dir=%s error=%s", path.c_str(), acl::last_serror());
        return false;
    }

    wt* dbp = new wt(cache_max_);
    if (!dbp->open(path.c_str())) {
        delete dbp;
        return false;
    }

    dbs_.push_back(dbp);
    logger("Open %s db ok", path.c_str());
    return true;
}

bool wdb::set(const std::string &key, const std::string &value) {
    unsigned n = acl_hash_crc32(key.c_str(), key.size()) % dbs_.size();
    auto dbp = dbs_[n];
    return dbp->set(key, value);
}

bool wdb::get(const std::string &key, std::string &value) {
    unsigned n = acl_hash_crc32(key.c_str(), key.size()) % dbs_.size();
    auto dbp = dbs_[n];
    return dbp->get(key, value);
}

bool wdb::del(const std::string &key) {
    unsigned n = acl_hash_crc32(key.c_str(), key.size()) % dbs_.size();
    auto dbp = dbs_[n];
    return dbp->del(key);
}

bool wdb::scan(db_cursor& cursor, std::vector<std::string> &keys, size_t max) {
    return false;
}

} // namespace pkv

#endif // HAS_WT
