#include "stdafx.h"
#include "paged_string.h"
#include "common/redis_integer.h"
#include "common/redis_decimal.h"
#include <cmath>

namespace pkv {
namespace dao {
db::page_root_result paged_string::increment_root(db::batch_entry& entry,
    long long delta, long long& value) {
    using choice = db::page_root_result;
    if (entry.found && entry.value.compare(0, 3, "PSR") != 0)
        return choice::fallback;
    string_root_header header;
    bool exists = entry.found;
    if (exists) {
        const auto status = string_page_codec::decode_header(entry.value,
            header);
        if (status != string_page_status::ok) {
            read_status_ = status == string_page_status::unsupported ?
                read_status::unsupported_format :
                status == string_page_status::out_of_memory ?
                    read_status::storage_error : read_status::invalid_record;
            return choice::rejected;
        }
        if (header.expires_at_ms >= 0 &&
            header.expires_at_ms <= unix_time_ms()) {
            exists = false; header = string_root_header();
        }
    }
    long long old = 0;
    if (exists && (header.length > 20 || !parse_redis_integer(
        entry.value.data() + header.payload_offset,
        static_cast<size_t>(header.length), old))) {
        read_status_ = read_status::invalid_integer;
        return choice::rejected;
    }
    if (!checked_integer_add(old, delta, value)) {
        read_status_ = read_status::integer_overflow;
        return choice::rejected;
    }
    if (string_page_codec::encode_integer(header.expires_at_ms, value,
        entry.value) !=
        string_page_status::ok) {
        read_status_ = read_status::storage_error;
        return choice::rejected;
    }
    entry.action = db::update_action::write;
    entry.clear_pages = true;
    return choice::ready;
}

bool paged_string::increment(shared_db& db, const std::string& key,
    long long delta, bool subtract, long long& value) {
    value = 0;
    if (subtract && delta == LLONG_MIN) {
        reset(); read_status_ = read_status::decrement_overflow; return false;
    }
    if (subtract) delta = -delta;
    auto change = [&](string_store& store) {
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
    };
    auto root = [&](db::batch_entry& entry) {
        return increment_root(entry, delta, value);
    };
    const operation operation_view(std::ref(change));
    const db::page_root_callback root_view(std::ref(root));
    const bool ok = run_callback(db, key, operation_view, &root_view);
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
