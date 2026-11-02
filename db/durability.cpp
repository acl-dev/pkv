#include "stdafx.h"
#include "db.h"

namespace pkv {
db::sync_status db::sync_key(const std::string& owner) {
    if (!dbsupports_sync()) return sync_status::unsupported;
    if (!opened_) return sync_status::error;
    try {
        execution_guard admission;
        auto& mutex = key_mutex(owner);
        if (!mutex.lock()) return sync_status::error;
        std::unique_lock<acl::fiber_mutex> guard(mutex, std::adopt_lock);
        return dbsync_key(owner);
    } catch (const std::bad_alloc&) { return sync_status::error; }
}
} // namespace pkv
