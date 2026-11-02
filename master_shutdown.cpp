#include "stdafx.h"
#include "master_service.h"
#include "common/execution_gate.h"
#include "db/database_pool.h"
#include "db/expiry_service.h"

void master_service::shutdown(int received) {
    if (received) logger("Shutdown requested by signal %d", received);
    else logger("Shutdown requested by service exit callback");
    std::unique_ptr<pkv::expiry_service> expiry;
    {
        std::lock_guard<std::mutex> guard(lifecycle_lock_);
        stopping_ = true;
        expiry.swap(expiry_);
    }
    // Expiry can be waiting for command admission: join before freezing it.
    expiry.reset();
    pkv::execution_guard admission(true);
    const bool closed = databases_ && databases_->close_for_exit();
    if (!closed) logger_error("Storage shutdown failed");
    else logger("Storage shutdown completed");
    std::fflush(stdout); std::fflush(stderr);
    // Retain exclusive admission until process death. ACL's detached workers
    // still own scheduler descriptors; never close those from a signal handler
    // or release admission after closing the native stores.
    _exit(closed ? 0 : 1);
}
