#include "stdafx.h"
#include "paged_list.h"
#include "native_import.h"
#include "common/redis_integer.h"

namespace pkv {
namespace dao {

bool paged_list::store_result(list_store& store) {
    switch (store.error()) {
    case list_store::status::ok: return true;
    case list_store::status::unsupported:
        read_status_ = read_status::unsupported_format; break;
    case list_store::status::storage:
        read_status_ = read_status::storage_error; break;
    case list_store::status::too_large:
        read_status_ = read_status::string_too_large; break;
    default: read_status_ = read_status::invalid_record; break;
    }
    return false;
}

bool paged_list::batch_result(db::batch_status status) {
    if (status == db::batch_status::error)
        read_status_ = read_status::storage_error;
    else if (status == db::batch_status::unsupported)
        read_status_ = read_status::unsupported_operation;
    if (status != db::batch_status::ok) return false;
    read_status_ = read_status::ok;
    return true;
}

bool paged_list::import(list_store& store) {
    return import_list(*view_, store, read_status_);
}

bool paged_list::open(shared_db& db, page_transaction& transaction,
    const std::string& key, list_store& store, long long now) {
    auto& root = *transaction.roots().find(key);
    if (!root.found) return store.open() || store_result(store);
    if (!decode(db, root.value, now)) {
        if (read_status_ != read_status::missing) return false;
        root.action = db::update_action::erase;
        return store.open() || store_result(store);
    }
    if (view_->header().kind != value_kind::list) {
        read_status_ = read_status::wrong_type;
        return false;
    }
    if (view_->paged_payload()) return store.open() || store_result(store);
    if (!import(store)) return false;
    if (!store.finish()) return store_result(store);
    root.maintenance_only = true;
    return true;
}

bool paged_list::run(shared_db& db, const std::string& key,
    const operation& callback) {
    reset();
    const auto result = db->transact_pages({key}, [&](page_transaction& tx) {
        list_store store(tx, key);
        if (!open(db, tx, key, store, unix_time_ms())) return false;
        if (!callback(store)) {
            if (store.error() != list_store::status::ok) store_result(store);
            return false;
        }
        return store.finish() || store_result(store);
    });
    return batch_result(result);
}

bool paged_list::push(shared_db& db, const std::string& key,
    const std::vector<std::string>& values, bool left, bool only_existing,
    long long& length) {
    length = 0;
    const bool ok = run(db, key, [&](list_store& store) {
        if (only_existing && !store.exists()) return true;
        if (!store.push(values, left)) return false;
        length = static_cast<long long>(store.metadata().elements);
        return true;
    });
    if (!ok) length = 0;
    return ok;
}

bool paged_list::pop(shared_db& db, const std::string& key, bool left,
    long long count, list_result& out) {
    out = list_result();
    if (count < 0) {
        reset(); read_status_ = read_status::invalid_integer; return false;
    }
    const bool ok = run(db, key, [&](list_store& store) {
        out.found = store.exists();
        return store.pop(static_cast<uint64_t>(count), left, &out.values);
    });
    if (!ok) out = list_result();
    return ok;
}

bool paged_list::length(shared_db& db, const std::string& key, long long& out) {
    out = 0;
    const bool ok = run(db, key, [&](list_store& store) {
        out = static_cast<long long>(store.metadata().elements);
        return true;
    });
    if (!ok) out = 0;
    return ok;
}

bool paged_list::range(shared_db& db, const std::string& key, long long start,
    long long end, std::vector<std::string>& out) {
    out.clear();
    const bool ok = run(db, key, [&](list_store& store) {
        return store.range(start, end, out);
    });
    if (!ok) out.clear();
    return ok;
}

bool paged_list::index(shared_db& db, const std::string& key,
    const std::string& index_text, list_result& out) {
    out = list_result();
    const bool ok = run(db, key, [&](list_store& store) {
        if (!store.exists()) return true;
        long long position;
        if (!parse_redis_integer(index_text, position)) {
            read_status_ = read_status::invalid_integer; return false;
        }
        std::string value;
        if (!store.index(position, value, out.found)) return false;
        if (out.found) out.values.push_back(std::move(value));
        return true;
    });
    if (!ok) out = list_result();
    return ok;
}

bool paged_list::set(shared_db& db, const std::string& key,
    const std::string& index_text, const std::string& value) {
    return run(db, key, [&](list_store& store) {
        if (!store.exists()) {
            read_status_ = read_status::no_key; return false;
        }
        long long position;
        if (!parse_redis_integer(index_text, position)) {
            read_status_ = read_status::invalid_integer; return false;
        }
        bool found;
        if (!store.replace(position, value, found)) return false;
        if (!found) { read_status_ = read_status::index_range; return false; }
        return true;
    });
}

bool paged_list::trim(shared_db& db, const std::string& key, long long start,
    long long end) {
    return run(db, key, [&](list_store& store) {
        return store.trim(start, end);
    });
}

bool paged_list::insert(shared_db& db, const std::string& key, bool before,
    const std::string& pivot, const std::string& value, long long& length) {
    length = 0;
    const bool ok = run(db, key, [&](list_store& store) {
        return store.insert(before, pivot, value, length);
    });
    if (!ok) length = 0;
    return ok;
}

bool paged_list::remove(shared_db& db, const std::string& key, long long count,
    const std::string& value, long long& removed) {
    removed = 0;
    if (count == LLONG_MIN) {
        reset(); read_status_ = read_status::list_count_range; return false;
    }
    const bool ok = run(db, key, [&](list_store& store) {
        return store.remove(count, value, removed);
    });
    if (!ok) removed = 0;
    return ok;
}

bool paged_list::positions(shared_db& db, const std::string& key,
    const std::string& value, const list_search& options,
    std::vector<long long>& out) {
    out.clear();
    const bool ok = run(db, key, [&](list_store& store) {
        return store.positions(value, options, out);
    });
    if (!ok) out.clear();
    return ok;
}

bool paged_list::move(shared_db& db, const std::string& source,
    const std::string& destination, bool from_left, bool to_left,
    list_result& out) {
    return move_checked(db, source, destination, from_left, to_left, out, {});
}
bool paged_list::move_checked(shared_db& db, const std::string& source,
    const std::string& destination, bool from_left, bool to_left,
    list_result& out, const key_predicate& eligible,
    const before_probe& before) {
    reset(); out = list_result();
    const auto result = db->transact_pages({source, destination},
        [&](page_transaction& tx) {
        if (before && !before()) return true;
        const auto now = unix_time_ms();
        list_store from(tx, source);
        if (!open(db, tx, source, from, now)) return false;
        if (!from.exists()) return from.finish() || store_result(from);
        if (eligible && !eligible(source)) return true;
        std::string value;
        if (source == destination) {
            if (!from.move_to(from, from_left, to_left, value, out.found))
                return store_result(from);
        } else {
            list_store to(tx, destination);
            if (!open(db, tx, destination, to, now)) return false;
            if (!from.move_to(to, from_left, to_left, value, out.found)) {
                store_result(from);
                store_result(to);
                return false;
            }
            if (!to.finish()) return store_result(to);
        }
        if (out.found) out.values.push_back(std::move(value));
        return from.finish() || store_result(from);
    });
    if (!batch_result(result)) { out = list_result(); return false; }
    return true;
}

bool paged_list::mpop(shared_db& db, const std::vector<std::string>& keys,
    bool left, long long count, std::string& selected_key, list_result& out) {
    return mpop_checked(db, keys, left, count, selected_key, out, {});
}
bool paged_list::mpop_checked(shared_db& db,
    const std::vector<std::string>& keys, bool left, long long count,
    std::string& selected_key, list_result& out, const key_predicate& eligible,
    const before_probe& before) {
    reset(); out = list_result(); selected_key.clear();
    if (count <= 0 || keys.empty()) {
        read_status_ = read_status::invalid_integer; return false;
    }
    const auto result = db->transact_pages(keys, [&](page_transaction& tx) {
        if (before && !before()) return true;
        const auto now = unix_time_ms();
        for (const auto& key : keys) {
            list_store store(tx, key);
            if (!open(db, tx, key, store, now)) return false;
            if (!store.exists()) {
                if (!store.finish()) return store_result(store);
                continue;
            }
            if (eligible && !eligible(key)) continue;
            out.found = true;
            selected_key = key;
            if (!store.pop(static_cast<uint64_t>(count), left, &out.values))
                return store_result(store);
            return store.finish() || store_result(store);
        }
        return true;
    });
    if (!batch_result(result)) {
        out = list_result(); selected_key.clear(); return false;
    }
    return true;
}

} // namespace dao
} // namespace pkv
