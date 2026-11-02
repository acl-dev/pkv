#include "stdafx.h"
#include "paged_hash.h"
#include "common/redis_decimal.h"
#include <cmath>

namespace pkv {
namespace dao {

bool paged_hash::hincrbyfloat(shared_db& db, const std::string& key,
    const std::string& name, const std::string& delta, std::string& value) {
    reset(); value.clear();
    long double increment;
    if (!parse_redis_decimal(delta, increment)) {
        read_status_ = read_status::invalid_score; return false;
    }
    if (!std::isfinite(increment)) {
        read_status_ = read_status::nonfinite_increment; return false;
    }
    std::string result;
    const bool ok = run(db, key, [&](map_store& store) {
        std::string bytes;
        bool found, exceeded, added;
        if (!store.get_bounded(name, redis_decimal_limit - 1, bytes, found,
            exceeded)) return false;
        long double old = 0;
        if (found && (exceeded || !parse_redis_decimal(bytes, old))) {
            read_status_ = read_status::invalid_hash_float; return false;
        }
        const auto sum = old + increment;
        if (!std::isfinite(sum)) {
            read_status_ = read_status::nonfinite_result; return false;
        }
        if (!format_redis_decimal(sum, result)) {
            read_status_ = read_status::storage_error; return false;
        }
        return store.put(name, result, false, added);
    });
    if (ok) value.swap(result);
    return ok;
}

} // namespace dao
} // namespace pkv
