#include "stdafx.h"

#ifdef HAS_ROCKSDB

#include "rocksdb/db.h"
#include "rocksdb/slice.h"
#include "rocksdb/options.h"

#include "rdb.h"

using namespace rocksdb;

namespace pkv {

rdb::rdb() : db_(nullptr) {}

rdb::~rdb() {
    delete db_;
    logger("rdb in %s closed", path_.c_str());
}

bool rdb::open(const char* path) {
    path_ = path;
    path_ += "/rdb";

    if (acl_make_dirs(path_.c_str(), 0755) == -1) {
        logger_error("create %s error=%s", path_.c_str(), acl::last_serror());
        return false;
    }

    Options options;
    options.IncreaseParallelism();
    // options.OptimizeLevelStyleCompaction();
    options.create_if_missing = true;
    Status s = DB::Open(options, path_, &db_);
    if (!s.ok()) {
        logger_error("open %s db error: %s", path_.c_str(), s.getState());
        return false;
    }

    return true;
}

bool rdb::set(const std::string& key, const std::string& value) {
    Status s = db_->Put(WriteOptions(), key, value);
    if (!s.ok()) {
        logger_error("put to %s error: %s, key=%s",
	    path_.c_str(), s.getState(), key.c_str());
        return false;
    }

    return true;
}

bool rdb::get(const std::string& key, std::string& value) {
    Status s = db_->Get(ReadOptions(), key, &value);
    if (!s.ok()) {
        if (!s.IsNotFound()) {
            logger_error("get from %s error: %s, key=%s, data=%zd",
                path_.c_str(), s.getState(), key.c_str(), value.size());
        }
        return false;
    }

    return true;
}

bool rdb::del(const std::string& key) {
    Status s = db_->Delete(WriteOptions(), key);
    if (!s.ok()) {
        logger_error("del from %s error: %s", path_.c_str(), s.getState());
        return false;
    }

    return true;
}

bool rdb::scan(const std::string& seek_key, std::vector<std::string>& keys,
       size_t max) {
    rocksdb::Iterator* it = db_->NewIterator(rocksdb::ReadOptions());
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
        keys.emplace_back(key);
        it->Next();
    }

    return true;
}

} // namespace pkv

#endif // HAS_ROCKSDB
