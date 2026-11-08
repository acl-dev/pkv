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
    auto increment_error = read_status::ok;
    const auto transform = [&](const std::string* bytes, bool exceeded,
        std::string& out) {
        long double old = 0;
        if (bytes && (exceeded || !parse_redis_decimal(*bytes, old))) {
            increment_error = read_status::invalid_hash_float;
            return false;
        }
        const auto sum = old + increment;
        if (!std::isfinite(sum)) {
            increment_error = read_status::nonfinite_result;
            return false;
        }
        if (!format_redis_decimal(sum, out)) {
            increment_error = read_status::storage_error;
            return false;
        }
        result = out;
        return true;
    };
    const bool ok = transform_field(db, key, name, redis_decimal_limit - 1,
        std::cref(transform));
    if (ok) value.swap(result);
    else if (increment_error != read_status::ok) read_status_ = increment_error;
    return ok;
}

} // namespace dao
} // namespace pkv
