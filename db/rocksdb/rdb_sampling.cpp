#include "stdafx.h"
#ifdef HAS_ROCKSDB
#include "rdb_storage.h"

namespace pkv {
db::scan_status rdb::dbvisit_keys(const key_visitor& visit) {
    for (const auto& store : stores_) {
        std::unique_ptr<rocksdb::Iterator> keys(store->data->NewIterator(
            rocksdb::ReadOptions(), store->scan));
        for (keys->SeekToFirst(); keys->Valid(); keys->Next()) {
            const auto name = keys->key();
            if (name.empty()) return scan_status::error;
            const std::string key(name.data() + 1, name.size() - 1);
            if (!visit(key)) return scan_status::ok;
        }
        if (!keys->status().ok()) return scan_status::error;
    }
    return scan_status::ok;
}
} // namespace pkv
#endif
