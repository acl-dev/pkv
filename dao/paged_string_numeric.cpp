#include "stdafx.h"
#include "paged_string.h"
#include "common/redis_integer.h"
#include "common/redis_decimal.h"
#include <cmath>

namespace pkv {
namespace dao {
bool paged_string::increment(shared_db& db, const std::string& key,
    long long delta, bool subtract, long long& value) {
    value = 0;
    if (subtract && delta == LLONG_MIN) {
        reset(); read_status_ = read_status::decrement_overflow; return false;
    }
    if (subtract) delta = -delta;
    const bool ok = run(db, key, [&](string_store& store) {
        std::string bytes;
        long long old = 0;
        if (store.metadata().length > 20) {
            read_status_ = read_status::invalid_integer; return false;
        }
        if (!store.read(bytes)) return false;
        if (store.exists() && !parse_redis_integer(bytes, old)) {
            read_status_ = read_status::invalid_integer; return false;
        }
        if (!checked_integer_add(old, delta, value)) {
            read_status_ = read_status::integer_overflow; return false;
        }
        return store.assign(std::to_string(value));
    });
    if (!ok) value = 0;
    return ok;
}
bool paged_string::increment_float(shared_db& db, const std::string& key,
    const std::string& delta, std::string& value) {
    value.clear();
    std::string out;
    const bool ok = run(db, key, [&](string_store& store) {
        long double old = 0, increment;
        std::string bytes;
        if (store.metadata().length >= redis_decimal_limit) {
            read_status_ = read_status::invalid_score; return false;
        }
        if (!store.read(bytes)) return false;
        if ((store.exists() && !parse_redis_decimal(bytes, old)) ||
            !parse_redis_decimal(delta, increment)) {
            read_status_ = read_status::invalid_score; return false;
        }
        const auto sum = old + increment;
        if (!std::isfinite(sum)) {
            read_status_ = read_status::nonfinite_result; return false;
        }
        if (!format_redis_decimal(sum, out)) {
            read_status_ = read_status::storage_error; return false;
        }
        return store.assign(out);
    });
    if (ok) value.swap(out);
    return ok;
}
bool paged_string::getex(shared_db& db, const std::string& key,
    const get_options& options, string_result& out) {
    out = string_result();
    const bool ok = run(db, key, [&](string_store& store) {
        if (!store.exists()) return true;
        if (!store.read(out.value)) return false;
        out.found = true;
        if (options.expiry == get_options::unchanged) return true;
        if (options.expiry == get_options::erase) return store.clear();
        if (options.expiry == get_options::persist) return store.set_expiry(-1);
        long long amount, deadline;
        if (!parse_redis_integer(options.amount, amount)) {
            read_status_ = read_status::invalid_integer; return false;
        }
        const bool seconds = options.expiry == get_options::ex ||
            options.expiry == get_options::exat;
        const bool absolute = options.expiry == get_options::exat ||
            options.expiry == get_options::pxat;
        if (amount <= 0 || !expiration_deadline(amount, seconds ? 1000 : 1,
            absolute ? 0 : now_, deadline)) {
            read_status_ = read_status::invalid_expiry; return false;
        }
        store.touch();
        return deadline <= now_ ? store.clear() :
            store.set_expiry(deadline);
    });
    if (!ok) out = string_result();
    return ok;
}
} // namespace dao
} // namespace pkv
