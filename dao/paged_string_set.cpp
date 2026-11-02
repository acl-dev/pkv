#include "stdafx.h"
#include "paged_string.h"
#include "string_comparison.h"

namespace pkv {
namespace dao {

bool paged_string::matches(page_transaction& tx, const std::string& key,
    const std::string& expected, bool& equal) {
    if (view_->header().kind != value_kind::string) {
        read_status_ = read_status::wrong_type; return false;
    }
    if (!view_->paged_payload()) {
        if (record_string_equal(*view_, expected, equal)) return true;
        read_status_ = read_status::invalid_record; return false;
    }
    string_store store(tx, key);
    if (!store.open()) return stored(store);
    if (string_equal_chunks(store.metadata().length, expected,
        [&](long long first, long long last, std::string& out) {
            return store.range(first, last, out);
        }, equal)) return true;
    if (store.error() != string_store::status::ok) return stored(store);
    read_status_ = read_status::invalid_record; return false;
}
bool paged_string::current(shared_db& db, page_transaction& tx,
    const std::string& key, std::string& out) {
    if (view_->header().kind != value_kind::string) {
        read_status_ = read_status::wrong_type; return false;
    }
    if (!view_->paged_payload()) {
        if (view_->string_value(out)) return true;
        read_status_ = read_status::invalid_record; return false;
    }
    string_store store(tx, key);
    return (store.open() && store.read(out)) || stored(store);
}
bool paged_string::set(shared_db& db, const std::string& key,
    const char* data) { return set(db, key, std::string(data)); }
bool paged_string::set(shared_db& db, const std::string& key,
    const std::string& data) {
    set_result out;
    return set(db, key, data, set_options(), out);
}
bool paged_string::set(shared_db& db, const std::string& key,
    const std::string& data, const set_options& options, set_result& out) {
    reset(); out = set_result();
    const bool compare = options.compares();
    if (compare && !options.comparison) {
        read_status_ = read_status::unsupported_operation; return false;
    }
    const auto status = db->transact_pages({key}, [&](page_transaction& tx) {
        const auto now = unix_time_ms();
        auto& entry = *tx.roots().find(key);
        const bool inspect = options.when != set_options::any ||
            options.keep_ttl || options.return_old;
        bool found = entry.found;
        int64_t expiry = options.expires_at_ms;
        if (inspect && found && !decode(db, entry.value, now)) {
            if (read_status_ != read_status::missing) return false;
            found = false; entry.action = db::update_action::erase;
        }
        if (options.return_old && found) {
            if (!current(db, tx, key, out.old_value)) return false;
            out.had_old = true;
        }
        if ((options.when == set_options::only_missing && found) ||
            (options.needs_existing() && !found))
            return true;
        if (compare && found) {
            bool equal;
            if (!comparison(tx, key, options,
                options.return_old ? &out.old_value : nullptr, equal))
                return false;
            if (equal != options.equality()) return true;
        }
        if (options.keep_ttl && found) expiry = view_->header().expires_at_ms;
        string_store store(tx, key);
        if (expiry >= 0 && expiry <= now) {
            store.initialize();
            if (!store.clear()) return stored(store);
        } else if (!store.assign(data) || !store.set_expiry(expiry))
            return stored(store);
        if (!store.finish()) return stored(store);
        out.applied = true;
        return true;
    });
    if (result(status)) return true;
    out = set_result(); return false;
}

bool paged_string::mget(shared_db& db, const std::vector<std::string>& keys,
    std::vector<string_result>& values) {
    reset(); values.clear();
    const auto status = db->transact_pages(keys, [&](page_transaction& tx) {
        const auto now = unix_time_ms();
        std::map<std::string, string_result> snapshot;
        for (const auto& entry : tx.roots()) {
            string_result value;
            if (entry.found && decode(db, entry.value, now)) {
                if (view_->header().kind == value_kind::string) {
                    if (!current(db, tx, entry.key, value.value)) return false;
                    value.found = true;
                }
            } else if (entry.found && read_status_ != read_status::missing)
                return false;
            snapshot.emplace(entry.key, std::move(value));
        }
        values.reserve(keys.size());
        for (const auto& key : keys) values.push_back(snapshot.at(key));
        return true;
    });
    if (result(status)) return true;
    values.clear(); return false;
}
bool paged_string::mset(shared_db& db,
    const std::map<std::string, std::string>& values, bool only_missing,
    bool& applied) {
    reset(); applied = false;
    std::vector<std::string> keys;
    for (const auto& value : values) {
        if (value.second.size() > redis_string_max) {
            read_status_ = read_status::string_too_large; return false;
        }
        keys.push_back(value.first);
    }
    const auto status = db->transact_pages(keys, [&](page_transaction& tx) {
        const auto now = unix_time_ms();
        if (only_missing) {
            for (const auto& entry : tx.roots()) {
                if (!entry.found) continue;
                if (decode(db, entry.value, now)) return true;
                if (read_status_ != read_status::missing) return false;
            }
        }
        for (const auto& value : values) {
            string_store store(tx, value.first);
            if (!store.assign(value.second) || !store.finish())
                return stored(store);
        }
        applied = true;
        return true;
    });
    if (result(status)) return true;
    applied = false; return false;
}
} // namespace dao
} // namespace pkv
