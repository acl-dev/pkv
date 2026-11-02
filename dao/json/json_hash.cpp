#include "stdafx.h"
#include "json_hash.h"
#include "common/redis_integer.h"

namespace pkv {
namespace dao {

bool json_hash::parse_fields() {
    record_ = value_record();
    record_.header.kind = value_kind::hash;
    if (!view_) return read_status_ == read_status::missing;
    if (view_->header().kind != value_kind::hash) {
        read_status_ = read_status::wrong_type;
        return false;
    }
    record_.header = view_->header();
    if (!view_->hash_values(record_.hash_value)) {
        read_status_ = read_status::invalid_record;
        return false;
    }
    return true;
}

bool json_hash::hgetall(shared_db& db, const std::string& key) {
    if (!read(db, key) && read_status_ != read_status::missing) return false;
    return parse_fields();
}

bool json_hash::hget(shared_db& db, const std::string& key,
        const std::string& name, std::string& value) {
    if (!read(db, key)) return false;
    if (view_->header().kind != value_kind::hash) {
        read_status_ = read_status::wrong_type;
        return false;
    }
    switch (view_->hash_field(name, value)) {
    case field_status::found: return true;
    case field_status::missing: read_status_ = read_status::missing; break;
    case field_status::invalid: read_status_ = read_status::invalid_record; break;
    }
    return false;
}

int json_hash::hmset(shared_db& db, const std::string& key,
        const std::map<std::string, std::string>& fields) {
    int added = 0;
    bool valid = true;
    reset();
    bool ok = db->update(key, [&](bool found, std::string& bytes) {
        if (found && !decode(db, bytes) && read_status_ != read_status::missing) {
            valid = false;
            return db::update_action::keep;
        }
        if (!parse_fields()) {
            valid = false;
            return db::update_action::keep;
        }
        for (const auto& field : fields) {
            auto inserted = record_.hash_value.emplace(field.first, field.second);
            if (inserted.second) ++added;
            else inserted.first->second = field.second;
        }
        if (!encode(db, record_, bytes)) {
            valid = false;
            return db::update_action::keep;
        }
        return db::update_action::write;
    });
    if (!ok) read_status_ = read_status::storage_error;
    return ok && valid ? added : -1;
}

bool json_hash::hset(shared_db& db, const std::string& key,
        const std::string& name, const std::string& value) {
    return hmset(db, key, {{name, value}}) >= 0;
}

int json_hash::hdel(shared_db& db, const std::string& key,
        const std::vector<std::string>& names) {
    int removed = 0;
    bool valid = true;
    reset();
    bool ok = db->update(key, [&](bool found, std::string& bytes) {
        if (!found) return db::update_action::keep;
        if (!decode(db, bytes)) {
            if (read_status_ == read_status::missing) return db::update_action::erase;
            valid = false;
            return db::update_action::keep;
        }
        if (!parse_fields()) {
            valid = false;
            return db::update_action::keep;
        }
        for (const auto& name : names) removed += record_.hash_value.erase(name);
        if (!removed) return db::update_action::keep;
        if (record_.hash_value.empty()) return db::update_action::erase;
        if (!encode(db, record_, bytes)) {
            valid = false;
            return db::update_action::keep;
        }
        return db::update_action::write;
    });
    if (!ok) read_status_ = read_status::storage_error;
    return ok && valid ? removed : -1;
}

int json_hash::hdel(shared_db& db, const std::string& key, const std::string& name) {
    return hdel(db, key, std::vector<std::string>{name});
}

bool json_hash::hmget(shared_db& db, const std::string& key,
        const std::vector<std::string>& names,
        std::map<std::string, std::string>& result) {
    result.clear();
    if (!read(db, key)) return read_status_ == read_status::missing;
    if (view_->header().kind != value_kind::hash) { read_status_ = read_status::wrong_type; return false; }
    for (const auto& name : names) {
        if (result.count(name)) continue;
        std::string value;
        const auto status = view_->hash_field(name, value);
        if (status == field_status::invalid) { read_status_ = read_status::invalid_record; return false; }
        if (status == field_status::found) result.emplace(name, std::move(value));
    }
    return true;
}

bool json_hash::hlen(shared_db& db, const std::string& key, long long& length) {
    length = 0;
    if (!read(db, key)) return read_status_ == read_status::missing;
    if (view_->header().kind != value_kind::hash) { read_status_ = read_status::wrong_type; return false; }
    size_t size;
    if (!view_->hash_size(size)) { read_status_ = read_status::invalid_record; return false; }
    length = static_cast<long long>(size);
    return true;
}

bool json_hash::hstrlen(shared_db& db, const std::string& key, const std::string& name,
        long long& length, bool& exists) {
    length = 0;
    exists = false;
    if (!read(db, key)) return read_status_ == read_status::missing;
    if (view_->header().kind != value_kind::hash) { read_status_ = read_status::wrong_type; return false; }
    size_t size = 0;
    const auto status = view_->hash_field_length(name, size);
    if (status == field_status::invalid) { read_status_ = read_status::invalid_record; return false; }
    exists = status == field_status::found;
    if (exists) length = static_cast<long long>(size);
    return true;
}

bool json_hash::hsetnx(shared_db& db, const std::string& key, const std::string& name,
        const std::string& value, bool& added) {
    reset();
    added = false;
    bool valid = true;
    const bool ok = db->update(key, [&](bool found, std::string& bytes) {
        if (found && !decode(db, bytes) && read_status_ != read_status::missing) {
            valid = false; return db::update_action::keep;
        }
        if (!parse_fields()) { valid = false; return db::update_action::keep; }
        if (!record_.hash_value.emplace(name, value).second) return db::update_action::keep;
        if (!encode(db, record_, bytes)) { valid = false; return db::update_action::keep; }
        added = true;
        return db::update_action::write;
    });
    if (!ok) read_status_ = read_status::storage_error;
    if (!ok || !valid) added = false;
    return ok && valid;
}

bool json_hash::hincrby(shared_db& db, const std::string& key, const std::string& name,
        long long delta, long long& value) {
    reset();
    bool valid = true;
    const bool ok = db->update(key, [&](bool found, std::string& bytes) {
        if (found && !decode(db, bytes) && read_status_ != read_status::missing) {
            valid = false; return db::update_action::keep;
        }
        if (!parse_fields()) { valid = false; return db::update_action::keep; }
        auto field = record_.hash_value.find(name);
        long long old = 0;
        if (field != record_.hash_value.end() && !parse_redis_integer(field->second, old)) {
            read_status_ = read_status::invalid_hash_integer;
            valid = false; return db::update_action::keep;
        }
        if (!checked_integer_add(old, delta, value)) {
            read_status_ = read_status::integer_overflow;
            valid = false; return db::update_action::keep;
        }
        record_.hash_value[name] = std::to_string(value);
        if (!encode(db, record_, bytes)) { valid = false; return db::update_action::keep; }
        return db::update_action::write;
    });
    if (!ok) read_status_ = read_status::storage_error;
    return ok && valid;
}

} // namespace dao
} // namespace pkv
