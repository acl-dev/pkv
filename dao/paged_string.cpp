#include "stdafx.h"
#include "paged_string.h"
#include "native_import.h"

namespace pkv {
namespace dao {

bool paged_string::stored(string_store& store) {
    read_status_ = string_read_status(store.error());
    return store.error() == string_store::status::ok;
}
bool paged_string::result(db::batch_status status) {
    if (status == db::batch_status::error)
        read_status_ = read_status::storage_error;
    else if (status == db::batch_status::unsupported)
        read_status_ = read_status::unsupported_operation;
    if (status != db::batch_status::ok) return false;
    read_status_ = read_status::ok;
    return true;
}
bool paged_string::import(string_store& store) {
    return import_string(*view_, store, read_status_);
}
bool paged_string::open(shared_db& db, page_transaction& tx,
    const std::string& key, string_store& store) {
    auto& entry = *tx.roots().find(key);
    if (!entry.found) return store.open() || stored(store);
    // Native roots already identify their type. Avoid decoding the inline
    // payload twice and allocating an unused generic metadata view.
    if (entry.value.compare(0, 3, "PSR") == 0) {
        if (!store.open()) return stored(store);
        const auto expiry = store.metadata().expires_at_ms;
        if (expiry >= 0 && expiry <= now_) {
            entry.action = db::update_action::erase;
            return store.open() || stored(store);
        }
        return true;
    }
    if (!decode(db, entry.value, now_)) {
        if (read_status_ != read_status::missing) return false;
        entry.action = db::update_action::erase;
        return store.open() || stored(store);
    }
    if (view_->header().kind != value_kind::string) {
        read_status_ = read_status::wrong_type; return false;
    }
    if (view_->paged_payload()) return store.open() || stored(store);
    if (!import(store)) return false;
    if (!store.finish()) { stored(store); return false; }
    entry.maintenance_only = true;
    return true;
}
bool paged_string::run_callback(shared_db& db, const std::string& key,
    const operation& callback, const db::page_root_callback* root) {
    reset();
    auto apply = [&](page_transaction& tx) {
        now_ = unix_time_ms();
        string_store store(tx, key);
        if (!open(db, tx, key, store)) return false;
        if (!callback(store)) {
            if (store.error() != string_store::status::ok) stored(store);
            return false;
        }
        return store.finish() || stored(store);
    };
    if (scope_) return result(scope_->transact(*db, key, std::ref(apply), root));
    const auto status = root ?
        db->transact_page(key, std::ref(apply), *root) :
        db->transact_page(key, std::ref(apply));
    return result(status);
}
bool paged_string::get(shared_db& db, const std::string& key,
    std::string& out) {
    reset();
    const auto direct = scope_ ? scope_->read_inline_string(*db, key, out) :
        db->read_inline_string(key, out);
    if (direct == db::inline_status::found) {
        read_status_ = read_status::ok; return true;
    }
    if (direct == db::inline_status::missing) {
        read_status_ = read_status::missing; return false;
    }
    if (direct == db::inline_status::error) {
        read_status_ = read_status::storage_error; return false;
    }
    bool found = false;
    std::string value;
    if (!run(db, key, [&](string_store& store) {
        found = store.exists(); return store.read(value);
    })) return false;
    if (!found) { read_status_ = read_status::missing; return false; }
    out.swap(value); return true;
}
bool paged_string::length(shared_db& db, const std::string& key,
    long long& length) {
    length = 0;
    const bool ok = run(db, key, [&](string_store& store) {
        length = static_cast<long long>(store.metadata().length); return true;
    });
    if (!ok) length = 0;
    return ok;
}
bool paged_string::getrange(shared_db& db, const std::string& key,
    long long start, long long end, std::string& out) {
    out.clear();
    if (!scope_) {
        const auto direct = db->read_string_range(key, start, end, out);
        if (direct != db::inline_status::fallback) {
            reset();
            if (direct == db::inline_status::error) {
                read_status_ = read_status::storage_error;
                return false;
            }
            read_status_ = read_status::ok;
            return true;
        }
    }
    std::string value;
    const bool ok = run(db, key, [&](string_store& store) {
        return store.range(start, end, value);
    });
    if (ok) out.swap(value);
    return ok;
}
bool paged_string::append(shared_db& db, const std::string& key,
    const std::string& suffix, long long& length) {
    length = 0;
    const bool ok = run(db, key, [&](string_store& store) {
        if (!store.append(suffix)) return false;
        length = static_cast<long long>(store.metadata().length); return true;
    });
    if (!ok) length = 0;
    return ok;
}
bool paged_string::setrange(shared_db& db, const std::string& key,
    long long offset, const std::string& value, long long& length) {
    length = 0;
    if (offset < 0) {
        reset(); read_status_ = read_status::offset_range; return false;
    }
    if (!scope_) {
        const auto direct = db->overwrite_string_range(key, static_cast<uint64_t>(offset),
            value, length);
        if (direct != db::inline_status::fallback) {
            reset();
            const bool written = direct == db::inline_status::found;
            read_status_ = written ? read_status::ok : read_status::storage_error;
            return written;
        }
    }
    const bool ok = run(db, key, [&](string_store& store) {
        if (!store.write(static_cast<uint64_t>(offset), value)) return false;
        length = static_cast<long long>(store.metadata().length); return true;
    });
    if (!ok) length = 0;
    return ok;
}
} // namespace dao
} // namespace pkv
