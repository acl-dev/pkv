#include "stdafx.h"
#include "paged_hash.h"
#include "native_import.h"
#include "common/redis_integer.h"

namespace pkv {
namespace dao {

bool paged_hash::store_result(map_store& store) {
    read_status_ = map_read_status(store.error());
    return store.error() == map_store::status::ok;
}

bool paged_hash::import(map_store& store) {
    return import_hash(*view_, store, read_status_);
}

bool paged_hash::open(shared_db& db, page_transaction& transaction,
    const std::string& key, map_store& store) {
    auto& root = *transaction.roots().find(key);
    if (!root.found) return store.open() || store_result(store);
    if (!decode(db, root.value)) {
        if (read_status_ != read_status::missing) return false;
        root.action = db::update_action::erase;
        return store.open() || store_result(store);
    }
    if (view_->header().kind != value_kind::hash) {
        read_status_ = read_status::wrong_type; return false;
    }
    if (view_->paged_payload()) return store.open() || store_result(store);
    if (!import(store)) return false;
    if (!store.finish()) return store_result(store);
    root.maintenance_only = true;
    return true;
}

bool paged_hash::run(shared_db& db, const std::string& key,
    const operation& callback) {
    reset(); fields_.clear();
    const auto result = db->transact_pages({key}, [&](page_transaction& tx) {
        map_store store(tx, key);
        if (!open(db, tx, key, store)) return false;
        if (!callback(store)) {
            if (store.error() != map_store::status::ok) store_result(store);
            return false;
        }
        return store.finish() || store_result(store);
    });
    if (result == db::batch_status::error)
        read_status_ = read_status::storage_error;
    else if (result == db::batch_status::unsupported)
        read_status_ = read_status::unsupported_operation;
    if (result != db::batch_status::ok) { fields_.clear(); return false; }
    read_status_ = read_status::ok;
    return true;
}

int paged_hash::hmset(shared_db& db, const std::string& key,
    const std::map<std::string, std::string>& fields) {
    int count = 0;
    const bool ok = run(db, key, [&](map_store& store) {
        for (const auto& field : fields) {
            bool added;
            if (!store.put(field.first, field.second, false, added))
                return false;
            if (added) ++count;
        }
        return true;
    });
    return ok ? count : -1;
}

bool paged_hash::hset(shared_db& db, const std::string& key,
    const std::string& name, const std::string& value) {
    return hmset(db, key, {{name, value}}) >= 0;
}

bool paged_hash::hget(shared_db& db, const std::string& key,
    const std::string& name, std::string& value) {
    std::string result;
    bool found = false;
    if (!run(db, key, [&](map_store& store) {
        return store.get(name, result, found);
    })) return false;
    if (!found) { read_status_ = read_status::missing; return false; }
    value.swap(result);
    return true;
}

int paged_hash::hdel(shared_db& db, const std::string& key,
    const std::vector<std::string>& names) {
    int count = 0;
    const bool ok = run(db, key, [&](map_store& store) {
        for (const auto& name : names) {
            bool removed;
            if (!store.erase(name, removed)) return false;
            if (removed) ++count;
        }
        return true;
    });
    return ok ? count : -1;
}

int paged_hash::hdel(shared_db& db, const std::string& key,
    const std::string& name) {
    return hdel(db, key, std::vector<std::string>{name});
}

bool paged_hash::hmget(shared_db& db, const std::string& key,
    const std::vector<std::string>& names,
    std::map<std::string, std::string>& result) {
    result.clear();
    const bool ok = run(db, key, [&](map_store& store) {
        for (const auto& name : names) {
            if (result.count(name)) continue;
            std::string value;
            bool found;
            if (!store.get(name, value, found)) return false;
            if (found) result.emplace(name, std::move(value));
        }
        return true;
    });
    if (!ok) result.clear();
    return ok;
}

bool paged_hash::hgetall(shared_db& db, const std::string& key) {
    return hgetall(db, key, true);
}

bool paged_hash::hgetall(shared_db& db, const std::string& key,
    bool with_values) {
    return run(db, key, [&](map_store& store) {
        if (!store.visit([&](const std::string& name,
            const std::string& value) {
            fields_.emplace(name, value);
            return true;
        }, with_values)) return false;
        if (fields_.size() != store.metadata().elements) {
            read_status_ = read_status::invalid_record; return false;
        }
        return true;
    });
}

bool paged_hash::hsetnx(shared_db& db, const std::string& key,
    const std::string& name, const std::string& value, bool& added) {
    added = false;
    const bool ok = run(db, key, [&](map_store& store) {
        return store.put(name, value, true, added);
    });
    if (!ok) added = false;
    return ok;
}

bool paged_hash::hincrby(shared_db& db, const std::string& key,
    const std::string& name, long long delta, long long& value) {
    value = 0;
    const bool ok = run(db, key, [&](map_store& store) {
        std::string bytes;
        bool found, added;
        if (!store.get(name, bytes, found)) return false;
        long long old = 0;
        if (found && !parse_redis_integer(bytes, old)) {
            read_status_ = read_status::invalid_hash_integer; return false;
        }
        if (!checked_integer_add(old, delta, value)) {
            read_status_ = read_status::integer_overflow; return false;
        }
        return store.put(name, std::to_string(value), false, added);
    });
    if (!ok) value = 0;
    return ok;
}

bool paged_hash::hlen(shared_db& db, const std::string& key,
    long long& length) {
    length = 0;
    const bool ok = run(db, key, [&](map_store& store) {
        length = static_cast<long long>(store.metadata().elements);
        return true;
    });
    if (!ok) length = 0;
    return ok;
}

bool paged_hash::hstrlen(shared_db& db, const std::string& key,
    const std::string& name, long long& length, bool& exists) {
    length = 0; exists = false;
    const bool ok = run(db, key, [&](map_store& store) {
        uint64_t bytes;
        if (!store.length(name, bytes, exists)) return false;
        length = static_cast<long long>(bytes);
        return true;
    });
    if (!ok) { length = 0; exists = false; }
    return ok;
}

} // namespace dao
} // namespace pkv
