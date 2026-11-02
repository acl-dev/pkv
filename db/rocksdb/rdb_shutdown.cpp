#include "stdafx.h"
#ifdef HAS_ROCKSDB
#include "rdb_storage.h"

namespace pkv {
bool rdb::dbclose_for_exit() {
    bool ok = true;
    for (const auto& storage : stores_) {
        const auto status = storage->data->FlushWAL(true);
        if (!status.ok()) {
            logger_error("RocksDB shutdown sync failed: %s",
                status.ToString().c_str());
            ok = false;
        }
    }
    stores_.clear();
    return ok;
}
} // namespace pkv
#endif
