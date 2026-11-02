#include "stdafx.h"
#include "json_string.h"
#include "common/redis_decimal.h"
#include <cmath>

namespace pkv {
namespace dao {

bool json_string::increment_float(shared_db& db, const std::string& key,
    const std::string& delta, std::string& value) {
    value.clear();
    std::string result;
    const bool ok = mutate(db, key, [&](bool found, value_record& record,
        long long, db::update_action& action) {
        long double old = 0, increment;
        if ((found && !parse_redis_decimal(record.string_value, old)) ||
            !parse_redis_decimal(delta, increment)) {
            read_status_ = read_status::invalid_score; return false;
        }
        const auto sum = old + increment;
        if (!std::isfinite(sum)) {
            read_status_ = read_status::nonfinite_result; return false;
        }
        if (!format_redis_decimal(sum, result)) {
            read_status_ = read_status::storage_error; return false;
        }
        record.string_value = result;
        action = db::update_action::write;
        return true;
    });
    if (ok) value.swap(result);
    return ok;
}

} // namespace dao
} // namespace pkv
