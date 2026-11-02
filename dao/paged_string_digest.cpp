#include "stdafx.h"
#include "paged_string.h"
#include "string_comparison.h"

namespace pkv {
namespace dao {
bool paged_string::digest_current(page_transaction& tx,
    const std::string& key, std::string& out) {
    if (view_->header().kind != value_kind::string) {
        read_status_ = read_status::wrong_type; return false;
    }
    if (!view_->paged_payload()) {
        if (record_string_digest(*view_, out)) return true;
        read_status_ = read_status::invalid_record; return false;
    }
    string_store store(tx, key);
    if (!store.open()) return stored(store);
    if (string_digest_chunks(store.metadata().length,
        [&](long long first, long long last, std::string& bytes) {
            return store.range(first, last, bytes);
        }, out)) return true;
    if (store.error() != string_store::status::ok) return stored(store);
    read_status_ = read_status::invalid_record; return false;
}
bool paged_string::comparison(page_transaction& tx, const std::string& key,
    const set_options& options, const std::string* old, bool& equal) {
    if (view_->header().kind != value_kind::string) {
        read_status_ = read_status::wrong_type; return false;
    }
    if (options.digest()) {
        if (options.comparison->size() != 16) {
            read_status_ = read_status::invalid_digest; return false;
        }
        std::string actual;
        if (old) actual = string_digest::value(*old);
        else if (!digest_current(tx, key, actual)) return false;
        equal = string_digest::equal(*options.comparison, actual);
        return true;
    }
    if (old) { equal = *old == *options.comparison; return true; }
    return matches(tx, key, *options.comparison, equal);
}
bool paged_string::digest(shared_db& db, const std::string& key,
    string_result& out) {
    reset(); out = string_result();
    const auto status = db->transact_pages({key}, [&](page_transaction& tx) {
        auto& entry = *tx.roots().find(key);
        if (!entry.found) return true;
        if (!decode(db, entry.value, unix_time_ms())) {
            if (read_status_ != read_status::missing) return false;
            entry.action = db::update_action::erase; return true;
        }
        if (!digest_current(tx, key, out.value)) return false;
        out.found = true; return true;
    });
    if (result(status)) return true;
    out = string_result(); return false;
}
bool paged_string::delex(shared_db& db, const std::string& key,
    const set_options& options, long long& removed) {
    reset(); removed = 0;
    if (options.compares() && !options.comparison) {
        read_status_ = read_status::unsupported_operation; return false;
    }
    const auto status = db->transact_pages({key}, [&](page_transaction& tx) {
        auto& entry = *tx.roots().find(key);
        if (!entry.found) return true;
        if (!decode(db, entry.value, unix_time_ms())) {
            if (read_status_ != read_status::missing) return false;
            entry.action = db::update_action::erase; return true;
        }
        if (options.when != set_options::any) {
            if (view_->header().kind != value_kind::string) {
                read_status_ = read_status::delex_type; return false;
            }
            if (!options.compares()) {
                read_status_ = read_status::delex_condition; return false;
            }
            bool equal;
            if (!comparison(tx, key, options, nullptr, equal)) return false;
            if (equal != options.equality()) return true;
        }
        entry.action = db::update_action::erase;
        removed = 1; return true;
    });
    if (result(status)) return true;
    removed = 0; return false;
}
} // namespace dao
} // namespace pkv
