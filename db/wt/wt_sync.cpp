#include "stdafx.h"
#ifdef HAS_WT
#include "wt_sess.h"

namespace pkv {
db::sync_status wt_sess::sync() {
    if (!session_ || transaction_) return db::sync_status::error;
    const int result = session_->log_flush(session_, "sync=on");
    if (!result) return db::sync_status::ok;
    logger_error("WiredTiger WAL synchronization failed: %s",
        wiredtiger_strerror(result));
    return db::sync_status::error;
}
} // namespace pkv
#endif
