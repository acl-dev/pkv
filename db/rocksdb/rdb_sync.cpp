#include "stdafx.h"
#ifdef HAS_ROCKSDB
#include "rdb_storage.h"

namespace pkv {
db::sync_status rdb::dbsync_key(const std::string& owner) {
    if (stores_.empty()) return sync_status::error;
    const auto status = stores_[locate(owner)]->data->FlushWAL(true);
    if (status.ok()) return sync_status::ok;
    if (status.IsNotSupported()) return sync_status::unsupported;
    logger_error("RocksDB WAL synchronization failed: %s",
        status.ToString().c_str());
    return sync_status::error;
}
} // namespace pkv
#endif
