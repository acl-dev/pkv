#include "stdafx.h"
#include "paged_zset.h"

namespace pkv {
namespace dao {

bool paged_zset::select(shared_db& db, const std::string& key,
    const zset_query& query, std::vector<zset_member>& out) {
    out.clear();
    const bool ok = run(db, key, [&](zset_store& store) {
        return store.range(query, out);
    });
    if (!ok) out.clear();
    return ok;
}

bool paged_zset::range(shared_db& db, const std::string& key, long long start,
    long long end, bool reverse, std::vector<zset_member>& out) {
    zset_query query;
    query.start = start; query.end = end; query.reverse = reverse;
    return select(db, key, query, out);
}

bool paged_zset::count(shared_db& db, const std::string& key,
    const zset_query& query, long long& out) {
    out = 0;
    const bool ok = run(db, key, [&](zset_store& store) {
        return store.count(query, out);
    });
    if (!ok) out = 0;
    return ok;
}

bool paged_zset::erase(zset_store& store,
    const std::vector<zset_member>& values) {
    if (!values.empty() && values.size() == store.metadata().elements)
        return store.clear();
    for (const auto& value : values) {
        bool removed;
        if (!store.erase(value.member, removed)) return false;
        if (!removed) {
            read_status_ = read_status::invalid_record; return false;
        }
    }
    return true;
}

bool paged_zset::remove_range(shared_db& db, const std::string& key,
    const zset_query& query, long long& removed) {
    removed = 0;
    const bool ok = run(db, key, [&](zset_store& store) {
        std::vector<zset_member> values;
        if (!store.range(query, values) || !erase(store, values)) return false;
        removed = static_cast<long long>(values.size());
        return true;
    });
    if (!ok) removed = 0;
    return ok;
}

bool paged_zset::store_range(shared_db& db, const std::string& source,
    const std::string& destination, const zset_query& query,
    long long& stored) {
    reset(); stored = 0;
    const bool ok = result(db->transact_pages({source, destination},
        [&](page_transaction& tx) {
        std::vector<zset_member> values;
        {
            zset_store from(tx, source);
            if (!open(db, tx, source, from, unix_time_ms())) return false;
            if (!from.range(query, values)) {
                read_status_ = map_read_status(from.error()); return false;
            }
            if (!finish(from)) return false;
        }
        zset_store target(tx, destination);
        target.initialize();
        for (const auto& value : values) {
            bool added, changed;
            if (!target.put(value.member, value.score, added, changed))
                return finish(target);
        }
        stored = static_cast<long long>(values.size());
        return finish(target);
    }));
    if (!ok) stored = 0;
    return ok;
}

bool paged_zset::pop(shared_db& db, const std::string& key, long long count,
    bool maximum, std::vector<zset_member>& out) {
    reset(); out.clear();
    if (count < 0) {
        read_status_ = read_status::invalid_integer; return false;
    }
    const bool ok = run(db, key, [&](zset_store& store) {
        if (!count) return true;
        zset_query query;
        query.end = count - 1; query.reverse = maximum;
        return store.range(query, out) && erase(store, out);
    });
    if (!ok) out.clear();
    return ok;
}

bool paged_zset::mpop(shared_db& db, const std::vector<std::string>& keys,
    bool maximum, long long count, std::string& selected_key,
    zset_pop_result& out) {
    return mpop_checked(db, keys, maximum, count, selected_key, out, {}, {});
}
bool paged_zset::mpop_checked(shared_db& db,
    const std::vector<std::string>& keys, bool maximum, long long count,
    std::string& selected_key, zset_pop_result& out,
    const key_predicate& eligible, const before_probe& before) {
    reset(); out = zset_pop_result(); selected_key.clear();
    if (keys.empty() || count <= 0) {
        read_status_ = read_status::invalid_integer; return false;
    }
    const bool ok = result(db->transact_pages(keys, [&](page_transaction& tx) {
        if (before && !before()) return true;
        const auto now = unix_time_ms();
        for (const auto& key : keys) {
            if (eligible && !eligible(key)) continue;
            zset_store store(tx, key);
            if (!open(db, tx, key, store, now)) return false;
            if (!store.exists()) {
                if (!finish(store)) return false;
                continue;
            }
            zset_query query;
            query.end = count - 1; query.reverse = maximum;
            if (!store.range(query, out.values) || !erase(store, out.values)) {
                if (store.error() != zset_store::status::ok)
                    read_status_ = map_read_status(store.error());
                return false;
            }
            if (!finish(store)) return false;
            selected_key = key; out.found = true;
            return true;
        }
        return true;
    }));
    if (!ok) { out = zset_pop_result(); selected_key.clear(); }
    return ok;
}

} // namespace dao
} // namespace pkv
