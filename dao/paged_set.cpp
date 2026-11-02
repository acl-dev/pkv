#include "stdafx.h"
#include "paged_set.h"
#include "native_import.h"

namespace pkv {
namespace dao {

bool paged_set::result(db::batch_status status) {
    if (status == db::batch_status::error)
        read_status_ = read_status::storage_error;
    else if (status == db::batch_status::unsupported)
        read_status_ = read_status::unsupported_operation;
    if (status != db::batch_status::ok) return false;
    read_status_ = read_status::ok;
    return true;
}

bool paged_set::finish(map_store& store) {
    if (store.finish()) return true;
    read_status_ = map_read_status(store.error());
    return false;
}

bool paged_set::import(map_store& store) {
    return import_set(*view_, store, read_status_);
}

bool paged_set::open(shared_db& db, page_transaction& tx,
    const std::string& key, map_store& store, long long now) {
    auto& root = *tx.roots().find(key);
    if (!root.found) return store.open() || finish(store);
    if (!decode(db, root.value, now)) {
        if (read_status_ != read_status::missing) return false;
        root.action = db::update_action::erase;
        return store.open() || finish(store);
    }
    if (view_->header().kind != value_kind::set) {
        read_status_ = read_status::wrong_type; return false;
    }
    if (view_->paged_payload()) return store.open() || finish(store);
    if (!import(store)) return false;
    if (!store.finish()) return finish(store);
    root.maintenance_only = true;
    return true;
}

bool paged_set::run(shared_db& db, const std::string& key,
    const operation& callback) {
    reset();
    return result(db->transact_pages({key}, [&](page_transaction& tx) {
        map_store store(tx, key, map_kind::set);
        if (!open(db, tx, key, store, unix_time_ms())) return false;
        if (!callback(store)) {
            if (store.error() != map_store::status::ok)
                read_status_ = map_read_status(store.error());
            return false;
        }
        return finish(store);
    }));
}

bool paged_set::change(shared_db& db, const std::string& key,
    const std::vector<std::string>& members, bool add, long long& changed) {
    changed = 0;
    const bool ok = run(db, key, [&](map_store& store) {
        for (const auto& member : members) {
            bool updated;
            if (!(add ? store.put(member, "", true, updated) :
                store.erase(member, updated))) return false;
            if (updated) ++changed;
        }
        return true;
    });
    if (!ok) changed = 0;
    return ok;
}

bool paged_set::size(shared_db& db, const std::string& key,
    long long& count) {
    count = 0;
    const bool ok = run(db, key, [&](map_store& store) {
        count = static_cast<long long>(store.metadata().elements);
        return true;
    });
    if (!ok) count = 0;
    return ok;
}

bool paged_set::contains(map_store& store, const std::string& member,
    bool& found) {
    uint64_t bytes;
    if (store.length(member, bytes, found)) return true;
    read_status_ = map_read_status(store.error());
    return false;
}

bool paged_set::membership(shared_db& db, const std::string& key,
    const std::vector<std::string>& members, std::vector<long long>& out) {
    out.clear();
    const bool ok = run(db, key, [&](map_store& store) {
        for (const auto& member : members) {
            bool found;
            if (!contains(store, member, found)) return false;
            out.push_back(found ? 1 : 0);
        }
        return true;
    });
    if (!ok) out.clear();
    return ok;
}

bool paged_set::members(shared_db& db, const std::string& key,
    std::set<std::string>& out) {
    out.clear();
    const bool ok = run(db, key, [&](map_store& store) {
        return store.visit([&](const std::string& member, const std::string&) {
            out.insert(member); return true;
        }, false);
    });
    if (!ok) out.clear();
    return ok;
}

bool paged_set::move(shared_db& db, const std::string& source,
    const std::string& destination, const std::string& member, bool& moved) {
    reset(); moved = false;
    const bool ok = result(db->transact_pages({source, destination},
        [&](page_transaction& tx) {
        const auto now = unix_time_ms();
        map_store from(tx, source, map_kind::set);
        if (!open(db, tx, source, from, now)) return false;
        if (!from.exists()) return finish(from);
        bool found;
        if (!contains(from, member, found)) return false;
        if (source == destination) { moved = found; return finish(from); }
        map_store to(tx, destination, map_kind::set);
        // Validate destination even when the source member is absent.
        if (!open(db, tx, destination, to, now)) return false;
        if (found) {
            bool added;
            if (!to.put(member, "", true, added)) return finish(to);
            if (!from.erase(member, moved)) return finish(from);
        }
        return finish(from) && finish(to);
    }));
    if (!ok) moved = false;
    return ok;
}

bool paged_set::sources(shared_db& db, page_transaction& tx,
    const std::vector<std::string>& keys, stores& out) {
    const auto now = unix_time_ms();
    for (const auto& key : keys) {
        if (out.count(key)) continue;
        auto store = std::unique_ptr<map_store>(
            new map_store(tx, key, map_kind::set));
        if (!open(db, tx, key, *store, now)) return false;
        out.emplace(key, std::move(store));
    }
    return true;
}

} // namespace dao
} // namespace pkv
