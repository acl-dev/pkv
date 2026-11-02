#pragma once
#include "db.h"
#include <cstdlib>
#include <mutex>

namespace pkv {
class database_pool;
// Internal shared lock frame for recovery and the physical coordinator.
// Finish construction before activating decision_guard; load may allocate.
class move_access {
    friend class move_recovery;
    friend class move_storage;
    friend class move_memory;
    enum class kind { disk, memory };
    execution_guard admission_;
    shared_db source_, target_;
    std::unique_lock<acl::fiber_mutex> first_, second_;
    db::batch_status status_ = db::batch_status::error;
    db::lookup_status from_status_ = db::lookup_status::missing;
    db::lookup_status to_status_ = db::lookup_status::missing;
    std::string from_, to_;
    move_access(database_pool& pool, size_t source, size_t target,
        const std::string& owner, kind mode = kind::disk);
    bool load(const std::string& owner);
    struct decision_guard {
        bool completed = false;
        ~decision_guard() { if (!completed) std::abort(); }
    };
};
} // namespace pkv
