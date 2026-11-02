#include "stdafx.h"
#include "json_string.h"
#include "../string_comparison.h"

namespace pkv {
namespace dao {
bool json_string::comparison(const set_options& options,
    const std::string* old, bool& equal) {
    if (view_->header().kind != value_kind::string) {
        read_status_ = read_status::wrong_type; return false;
    }
    if (options.digest()) {
        if (options.comparison->size() != 16) {
            read_status_ = read_status::invalid_digest; return false;
        }
        std::string actual;
        if (old) actual = string_digest::value(*old);
        else if (!record_string_digest(*view_, actual)) {
            read_status_ = read_status::invalid_record; return false;
        }
        equal = string_digest::equal(*options.comparison, actual);
        return true;
    }
    if (old) { equal = *old == *options.comparison; return true; }
    if (record_string_equal(*view_, *options.comparison, equal)) return true;
    read_status_ = read_status::invalid_record; return false;
}
bool json_string::digest(shared_db& db, const std::string& key,
    string_result& out) {
    reset(); out = string_result();
    bool valid = true;
    const bool ok = db->update(key, [&](bool found, std::string& bytes) {
        if (!found) return db::update_action::keep;
        if (!decode(db, bytes)) {
            valid = read_status_ == read_status::missing;
            return valid ? db::update_action::erase : db::update_action::keep;
        }
        if (view_->header().kind != value_kind::string) {
            read_status_ = read_status::wrong_type; valid = false;
        } else if (!record_string_digest(*view_, out.value)) {
            read_status_ = read_status::invalid_record; valid = false;
        } else out.found = true;
        return db::update_action::keep;
    });
    if (!ok) read_status_ = read_status::storage_error;
    if (!ok || !valid) out = string_result();
    return ok && valid;
}
bool json_string::delex(shared_db& db, const std::string& key,
    const set_options& options, long long& removed) {
    reset(); removed = 0;
    if (options.compares() && !options.comparison) {
        read_status_ = read_status::unsupported_operation; return false;
    }
    bool valid = true;
    const bool ok = db->update(key, [&](bool found, std::string& bytes) {
        if (!found) return db::update_action::keep;
        if (!decode(db, bytes)) {
            valid = read_status_ == read_status::missing;
            return valid ? db::update_action::erase : db::update_action::keep;
        }
        if (options.when != set_options::any) {
            if (view_->header().kind != value_kind::string) {
                read_status_ = read_status::delex_type; valid = false;
            } else if (!options.compares()) {
                read_status_ = read_status::delex_condition; valid = false;
            }
            if (!valid) return db::update_action::keep;
            bool equal;
            if (!comparison(options, nullptr, equal)) {
                valid = false; return db::update_action::keep;
            }
            if (equal != options.equality()) return db::update_action::keep;
        }
        removed = 1; return db::update_action::erase;
    });
    if (!ok) read_status_ = read_status::storage_error;
    if (!ok || !valid) removed = 0;
    return ok && valid;
}
} // namespace dao
} // namespace pkv
