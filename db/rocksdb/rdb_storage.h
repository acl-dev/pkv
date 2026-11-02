#pragma once
#ifdef HAS_ROCKSDB
#include "rdb.h"
#include "db/disk_address.h"
#include <rocksdb/db.h>
#include <rocksdb/write_batch.h>
#include <string>
#include <atomic>
#include <stdexcept>

namespace pkv {

struct rdb::store {
    std::atomic<uint64_t> root_count{0};
    rocksdb::DB* data = nullptr;
    rocksdb::ColumnFamilyHandle* roots = nullptr;
    rocksdb::ColumnFamilyHandle* pages = nullptr;
    rocksdb::ColumnFamilyHandle* scan = nullptr;
    std::vector<rocksdb::ColumnFamilyHandle*> handles;
    ~store() {
        for (auto* handle : handles) delete handle;
        delete data;
    }
};

namespace rdb_address = disk_address;

} // namespace pkv
#endif
