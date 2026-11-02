#include "stdafx.h"
#include "db.h"
#include "expiry_index.h"

namespace pkv {
db::count_status db::keyspace(uint64_t& keys, uint64_t& expires,
    uint64_t& average) {
    execution_guard admission(true);
    keys = expires = average = 0;
    if ((!expiry_ || expiry_->dirty()) && rebuild_expiry() != scan_status::ok)
        return count_status::error;
    const auto status = key_count(keys);
    if (status != count_status::ok) return status;
    const auto ttl = expiry_->snapshot(execution_time_ms());
    expires = ttl.count; average = ttl.average;
    return count_status::ok;
}
} // namespace pkv
