#include "stdafx.h"
#include "json_key.h"
#include "dao/native_import.h"

namespace pkv {
namespace dao {
namespace {
bool expiry_matches(long long previous, long long deadline,
    unsigned conditions, bool persist) {
    if (persist) return previous >= 0;
    return !(((conditions & key::expire_nx) && previous >= 0) ||
        ((conditions & key::expire_xx) && previous < 0) ||
        ((conditions & key::expire_gt) &&
            (previous < 0 || deadline <= previous)) ||
        ((conditions & key::expire_lt) && previous >= 0 &&
            deadline >= previous));
}
} // namespace

bool json_key::set_expiry(shared_db& db, const std::string& key,
    long long deadline, unsigned conditions) {
    return update_expiry(db, key, deadline, conditions, false);
}
bool json_key::persist(shared_db& db, const std::string& key) {
    return update_expiry(db, key, -1, 0, true);
}

bool json_key::update_expiry(shared_db& db, const std::string& key,
    long long deadline, unsigned conditions, bool persist, bool native) {
    bool changed = false, valid = true, migrate = false;
    reset();
    const bool ok = db->update_root(key,
        [&](bool found, std::string& bytes, bool& preserve_pages) {
        if (!found) return db::update_action::keep;
        if (!decode(db, bytes)) {
            valid = read_status_ == read_status::missing;
            return valid ? db::update_action::erase : db::update_action::keep;
        }
        if (!expiry_matches(view_->header().expires_at_ms, deadline,
            conditions, persist)) return db::update_action::keep;
        if (!persist && deadline <= unix_time_ms()) {
            changed = true; return db::update_action::erase;
        }
        if (native && !view_->paged_payload()) {
            migrate = true; return db::update_action::keep;
        }
        if (!encode_expiry(db, bytes, deadline, preserve_pages)) {
            valid = false; return db::update_action::keep;
        }
        changed = true;
        return db::update_action::write;
    });
    if (!ok) read_status_ = read_status::storage_error;
    if (!ok || !valid) return false;
    if (!migrate) return changed;
    bool unsupported = false;
    if (migrate_expiry(db, key, deadline, conditions, persist, changed,
        unsupported)) return changed;
    // Nonpage engines and observers retain their legacy-compatible path.
    // Unsupported transactions commit nothing, so retry the current value.
    if (unsupported)
        return update_expiry(db, key, deadline, conditions, persist, false);
    return false;
}

bool json_key::migrate_expiry(shared_db& db, const std::string& key,
    long long deadline, unsigned conditions, bool persist, bool& changed,
    bool& unsupported) {
    changed = false; unsupported = false;
    const auto result = db->transact_pages({key}, [&](page_transaction& tx) {
        auto& root = *tx.roots().find(key);
        if (!root.found) return true;
        // Recheck under the new lock: another actor may replace the key
        // between the metadata probe and this transaction.
        if (!decode(db, root.value)) {
            if (read_status_ != read_status::missing) return false;
            root.action = db::update_action::erase; return true;
        }
        if (!expiry_matches(view_->header().expires_at_ms, deadline,
            conditions, persist)) return true;
        if (!persist && deadline <= unix_time_ms()) {
            root.action = db::update_action::erase;
            changed = true; return true;
        }
        if (!view_->paged_payload()) {
            if (!import_native(*view_, tx, key, read_status_) ||
                !decode(db, root.value)) return false;
        }
        bool preserve = false;
        if (!encode_expiry(db, root.value, deadline, preserve)) return false;
        root.action = db::update_action::write;
        root.maintenance_only = false;
        changed = true;
        return true;
    });
    unsupported = result == db::batch_status::unsupported;
    if (!page_result(result)) { changed = false; return false; }
    return true;
}

} // namespace dao
} // namespace pkv
