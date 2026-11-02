#include "stdafx.h"
#ifdef HAS_WT
#include "wdb.h"
#include "wt.h"

namespace pkv {
db::sync_status wdb::dbsync_key(const std::string& owner) {
    return dbs_.empty() ? sync_status::error : dbs_[locate(owner)]->sync();
}
} // namespace pkv
#endif
