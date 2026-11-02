#include "stdafx.h"
#include "common/redis_integer.h"
#include "json_string.h"
#include "../string_comparison.h"

namespace pkv {
namespace dao {

json_string::json_string() = default;

bool json_string::to_string(std::string& out) {
    value_record record;
    record.string_value = data_;
    return default_record_codec()->encode(record, out) == codec_status::ok;
}

void json_string::set_string(const char* data) {
    reset();
    data_ = data;
}

bool json_string::set(shared_db& db, const std::string& key, const char* data) {
    return set(db, key, std::string(data));
}

bool json_string::set(shared_db& db, const std::string& key, const std::string& data) {
    reset();
    value_record record;
    record.string_value = data;
    std::string bytes;
    if (!encode(db, record, bytes)) return false;
    if (!save(db, key, bytes)) {
        read_status_ = read_status::storage_error;
        return false;
    }
    return true;
}

bool json_string::set(shared_db& db, const std::string& key,
    const std::string& data, const set_options& options, set_result& result) {
    result = set_result();
    reset();
    const bool compare = options.compares();
    if (compare && !options.comparison) {
        read_status_ = read_status::unsupported_operation; return false;
    }
    value_record record;
    record.string_value = data;
    record.header.expires_at_ms = options.expires_at_ms;
    const bool needs_old = options.when != set_options::any ||
        options.return_old || options.keep_ttl;
    // Preserve the direct SET path when the previous value is irrelevant.
    if (!needs_old && (options.expires_at_ms < 0 ||
        options.expires_at_ms > unix_time_ms())) {
        std::string bytes;
        if (!encode(db, record, bytes)) return false;
        if (!save(db, key, bytes)) {
            read_status_ = read_status::storage_error; return false;
        }
        result.applied = true;
        return true;
    }
    bool valid = true;
    const bool ok = db->update(key, [&](bool raw_found, std::string& bytes) {
        bool found = raw_found;
        if (raw_found && needs_old && !decode(db, bytes)) {
            if (read_status_ == read_status::missing) found = false;
            else { valid = false; return db::update_action::keep; }
        }
        if (options.return_old && found) {
            if (view_->header().kind != value_kind::string) {
                read_status_ = read_status::wrong_type;
                valid = false;
                return db::update_action::keep;
            }
            if (!view_->string_value(result.old_value)) {
                read_status_ = read_status::invalid_record;
                valid = false;
                return db::update_action::keep;
            }
            result.had_old = true;
        }
        if ((options.when == set_options::only_missing && found) ||
            (options.needs_existing() && !found)) {
            return raw_found && !found ? db::update_action::erase :
                db::update_action::keep;
        }
        if (compare && found) {
            bool equal;
            if (!comparison(options,
                options.return_old ? &result.old_value : nullptr, equal)) {
                valid = false; return db::update_action::keep;
            }
            if (equal != options.equality()) return db::update_action::keep;
        }
        if (options.keep_ttl && found)
            record.header.expires_at_ms = view_->header().expires_at_ms;
        if (!encode(db, record, bytes)) {
            valid = false; return db::update_action::keep;
        }
        result.applied = true;
        return record.header.expires_at_ms >= 0 &&
            record.header.expires_at_ms <= unix_time_ms() ?
                db::update_action::erase : db::update_action::write;
    });
    if (!ok) read_status_ = read_status::storage_error;
    if (!ok || !valid) result = set_result();
    return ok && valid;
}

bool json_string::mget(shared_db& db, const std::vector<std::string>& keys,
        std::vector<string_result>& values) {
    reset();
    values.clear();
    const auto status = db->transact(keys, [&](db::batch_view entries) {
        const auto now = unix_time_ms();
        std::map<std::string, string_result> snapshot;
        for (auto& entry : entries) {
            string_result value;
            if (entry.found && decode(db, entry.value, now)) {
                // MGET returns nil for non-string values, unlike GET.
                if (view_->header().kind == value_kind::string) {
                    if (!view_->string_value(value.value)) {
                        read_status_ = read_status::invalid_record;
                        return false;
                    }
                    value.found = true;
                }
            } else if (entry.found && read_status_ != read_status::missing) return false;
            snapshot.emplace(entry.key, std::move(value));
        }
        values.reserve(keys.size());
        for (const auto& key : keys) values.push_back(snapshot.at(key));
        return true;
    });
    if (status == db::batch_status::error) read_status_ = read_status::storage_error;
    else if (status == db::batch_status::unsupported) read_status_ = read_status::unsupported_operation;
    return status == db::batch_status::ok;
}

bool json_string::mset(shared_db& db, const std::map<std::string, std::string>& values,
        bool only_missing, bool& applied) {
    reset();
    applied = false;
    // Encode every replacement before taking locks or changing any stored bytes.
    std::map<std::string, std::string> encoded;
    std::vector<std::string> keys;
    keys.reserve(values.size());
    for (const auto& value : values) {
        value_record record;
        record.string_value = value.second;
        std::string bytes;
        if (!encode(db, record, bytes)) return false;
        encoded.emplace(value.first, std::move(bytes));
        keys.push_back(value.first);
    }
    const auto status = db->transact(keys, [&](db::batch_view entries) {
        const auto now = unix_time_ms();
        if (only_missing) {
            for (const auto& entry : entries) {
                if (!entry.found) continue;
                if (decode(db, entry.value, now)) return true;
                if (read_status_ != read_status::missing) return false;
            }
        }
        for (auto& entry : entries) {
            entry.value = encoded.at(entry.key);
            entry.action = db::update_action::write;
        }
        applied = true;
        return true;
    });
    if (status == db::batch_status::error) read_status_ = read_status::storage_error;
    else if (status == db::batch_status::unsupported) read_status_ = read_status::unsupported_operation;
    if (status != db::batch_status::ok) applied = false;
    return status == db::batch_status::ok;
}

bool json_string::mutate(shared_db& db, const std::string& key, const mutator& callback) {
    reset();
    bool valid = true;
    const bool ok = db->update(key, [&](bool raw_found, std::string& bytes) {
        const auto now = unix_time_ms();
        bool found = raw_found;
        if (raw_found && !decode(db, bytes, now)) {
            if (read_status_ == read_status::missing) found = false;
            else { valid = false; return db::update_action::keep; }
        }
        value_record record;
        if (found) {
            if (view_->header().kind != value_kind::string) {
                read_status_ = read_status::wrong_type;
                valid = false;
                return db::update_action::keep;
            }
            if (!materialize(record)) { valid = false; return db::update_action::keep; }
        }
        db::update_action action = db::update_action::keep;
        if (!callback(found, record, now, action)) { valid = false; return db::update_action::keep; }
        if (action == db::update_action::write) {
            if (record.string_value.size() > redis_string_max) {
                read_status_ = read_status::string_too_large;
                valid = false;
                return db::update_action::keep;
            }
            if (!encode(db, record, bytes)) { valid = false; return db::update_action::keep; }
        }
        if (action == db::update_action::keep && raw_found && !found) return db::update_action::erase;
        return action;
    });
    if (!ok) read_status_ = read_status::storage_error;
    return ok && valid;
}

bool json_string::increment(shared_db& db, const std::string& key,
        long long delta, bool subtract, long long& value) {
    if (subtract && delta == LLONG_MIN) {
        reset();
        read_status_ = read_status::decrement_overflow;
        return false;
    }
    if (subtract) delta = -delta;
    return mutate(db, key, [&](bool found, value_record& record, long long, db::update_action& action) {
        long long old = 0;
        if (found && !parse_redis_integer(record.string_value, old)) {
            read_status_ = read_status::invalid_integer;
            return false;
        }
        if (!checked_integer_add(old, delta, value)) {
            read_status_ = read_status::integer_overflow;
            return false;
        }
        record.string_value = std::to_string(value);
        action = db::update_action::write;
        return true;
    });
}

bool json_string::append(shared_db& db, const std::string& key,
        const std::string& suffix, long long& length) {
    return mutate(db, key, [&](bool, value_record& record, long long, db::update_action& action) {
        if (suffix.size() > redis_string_max || record.string_value.size() > redis_string_max - suffix.size()) {
            read_status_ = read_status::string_too_large;
            return false;
        }
        record.string_value.append(suffix);
        length = static_cast<long long>(record.string_value.size());
        action = db::update_action::write;
        return true;
    });
}

bool json_string::setrange(shared_db& db, const std::string& key, long long offset,
        const std::string& value, long long& length) {
    if (offset < 0) { reset(); read_status_ = read_status::offset_range; return false; }
    return mutate(db, key, [&](bool, value_record& record, long long, db::update_action& action) {
        length = static_cast<long long>(record.string_value.size());
        if (value.empty()) return true;
        if (value.size() > redis_string_max || static_cast<unsigned long long>(offset) > redis_string_max - value.size()) {
            read_status_ = read_status::string_too_large;
            return false;
        }
        const auto end = static_cast<size_t>(offset) + value.size();
        if (record.string_value.size() < end) record.string_value.resize(end, '\0');
        record.string_value.replace(static_cast<size_t>(offset), value.size(), value);
        length = static_cast<long long>(record.string_value.size());
        action = db::update_action::write;
        return true;
    });
}

bool json_string::length(shared_db& db, const std::string& key, long long& length) {
    length = 0;
    if (!read(db, key)) return read_status_ == read_status::missing;
    if (view_->header().kind != value_kind::string) { read_status_ = read_status::wrong_type; return false; }
    size_t size;
    if (!view_->string_length(size)) { read_status_ = read_status::invalid_record; return false; }
    length = static_cast<long long>(size);
    return true;
}

bool json_string::getrange(shared_db& db, const std::string& key, long long start, long long end, std::string& out) {
    out.clear();
    if (!read(db, key)) return read_status_ == read_status::missing;
    if (view_->header().kind != value_kind::string) { read_status_ = read_status::wrong_type; return false; }
    if (!view_->string_range(start, end, out)) { read_status_ = read_status::invalid_record; return false; }
    return true;
}

bool json_string::getex(shared_db& db, const std::string& key, const get_options& options, string_result& result) {
    result = string_result();
    return mutate(db, key, [&](bool found, value_record& record, long long now, db::update_action& action) {
        if (!found) return true;
        result.found = true;
        if (options.expiry == get_options::unchanged || options.expiry == get_options::erase) {
            result.value = std::move(record.string_value);
            if (options.expiry == get_options::erase) action = db::update_action::erase;
            return true;
        }
        result.value = record.string_value;
        if (options.expiry == get_options::persist) {
            if (record.header.expires_at_ms < 0) return true;
            record.header.expires_at_ms = -1;
        } else {
            long long amount, deadline;
            if (!parse_redis_integer(options.amount, amount)) { read_status_ = read_status::invalid_integer; return false; }
            const bool seconds = options.expiry == get_options::ex || options.expiry == get_options::exat;
            const bool absolute = options.expiry == get_options::exat || options.expiry == get_options::pxat;
            if (amount <= 0 || !expiration_deadline(amount, seconds ? 1000 : 1, absolute ? 0 : now, deadline)) {
                read_status_ = read_status::invalid_expiry;
                return false;
            }
            record.header.expires_at_ms = deadline;
            if (deadline <= now) { action = db::update_action::erase; return true; }
        }
        action = db::update_action::write;
        return true;
    });
}

bool json_string::get(shared_db& db, const std::string& key, std::string& out) {
    if (!read(db, key)) return false;
    if (view_->header().kind != value_kind::string) {
        read_status_ = read_status::wrong_type;
        return false;
    }
    if (!view_->string_value(out)) {
        read_status_ = read_status::invalid_record;
        return false;
    }
    return true;
}

} // namespace dao
} // namespace pkv
