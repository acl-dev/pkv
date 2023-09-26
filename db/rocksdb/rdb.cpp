#include "stdafx.h"

#ifdef HAS_ROCKSDB

#include "rocksdb/db.h"
#include "rocksdb/options.h"

#include "../db_cursor.h"
#include "rdb_cursor.h"
#include "rdb.h"

using namespace rocksdb;

namespace pkv {

rdb::~rdb() {
    size_t i = 0;
    for (auto dbp : dbs_) {
        delete dbp;
	logger("db(%s) closed", paths_[i++].c_str());
    }

    logger("rdb closed");
}

bool rdb::dbopen(const char* paths) {
    acl::string buf(paths);
    auto& tokens = buf.split2(";, \t");
    for (auto& token : tokens) {
        std::string path = token;
        path += "/rdb";
        if (!open_one(path)) {
            return false;
        }
    }
    return true;
}

bool rdb::open_one(const std::string& path) {
    if (acl_make_dirs(path.c_str(), 0755) == -1) {
        logger_error("create %s error=%s", path.c_str(), acl::last_serror());
        return false;
    }

    rocksdb::DB* dbp;

    Options options;
    options.IncreaseParallelism();
    // options.OptimizeLevelStyleCompaction();
    options.create_if_missing = true;
    Status s = DB::Open(options, path, &dbp);
    if (!s.ok()) {
        logger_error("open %s db error: %s", path.c_str(), s.getState());
        return false;
    }

    dbs_.push_back(dbp);
    paths_.push_back(path);
    logger("open %s ok, db=%zd", path.c_str(), dbs_.size());
    return true;
}

bool rdb::dbset(const std::string& key, const std::string& value) {
    unsigned n = acl_hash_crc32(key.c_str(), key.size()) % dbs_.size();
    auto dbp = dbs_[n];

    Status s = dbp->Put(WriteOptions(), key, value);
    if (!s.ok()) {
        logger_error("put to %s error: %s, key=%s",
            paths_[n].c_str(), s.getState(), key.c_str());
        return false;
    }

    return true;
}

bool rdb::dbget(const std::string& key, std::string& value) {
    unsigned n = acl_hash_crc32(key.c_str(), key.size()) % dbs_.size();
    auto dbp = dbs_[n];

    Status s = dbp->Get(ReadOptions(), key, &value);
    if (!s.ok()) {
        if (!s.IsNotFound()) {
            logger_error("get from %s error: %s, key=%s, data=%zd",
                paths_[n].c_str(), s.getState(), key.c_str(), value.size());
        }
        return false;
    }

    return true;
}

bool rdb::dbdel(const std::string& key) {
    unsigned n = acl_hash_crc32(key.c_str(), key.size()) % dbs_.size();
    auto dbp = dbs_[n];

    Status s = dbp->Delete(WriteOptions(), key);
    if (!s.ok()) {
        logger_error("del from %s error: %s", paths_[n].c_str(), s.getState());
        return false;
    }

    return true;
}

db_cursor* rdb::dbcreate_cursor() {
    return new rdb_cursor();
}

bool rdb::dbscan(size_t idx, db_cursor& cur, std::vector<std::string>& keys,
      size_t max) {
    if (idx >= dbs_.size()) {
        return false;
    }

    auto dbp = dbs_[idx];
    auto& cursor = (rdb_cursor&) cur;
    auto& seek_key = cursor.get_seek_key();

    rocksdb::Iterator* it = dbp->NewIterator(rocksdb::ReadOptions());
    if (seek_key.empty()) {
        it->SeekToFirst();
    } else {
        it->Seek(seek_key);
        if (it->Valid()) {
            it->Next();
        } else {
            return true;
        }
    }

    for (size_t i = 0; i < max && it->Valid(); i++) {
        auto key = it->key().ToString();
        cursor.set_seek_key(key);
        keys.emplace_back(key);
        it->Next();
    }
    return true;
}

} // namespace pkv

#endif // HAS_ROCKSDB
