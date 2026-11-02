#include "stdafx.h"
#include "paged_zset.h"
#include "native_import.h"
#include <cmath>

namespace pkv {
namespace dao {

bool paged_zset::result(db::batch_status status) {
    if (status == db::batch_status::error)
        read_status_ = read_status::storage_error;
    else if (status == db::batch_status::unsupported)
        read_status_ = read_status::unsupported_operation;
    if (status != db::batch_status::ok) return false;
    read_status_ = read_status::ok;
    return true;
}

bool paged_zset::finish(zset_store& store) {
    if (store.finish()) return true;
    read_status_ = map_read_status(store.error());
    return false;
}

bool paged_zset::import(zset_store& store) {
    return import_zset(*view_, store, read_status_);
}

bool paged_zset::open(shared_db& db, page_transaction& tx,
    const std::string& key, zset_store& store, long long now) {
    auto& root = *tx.roots().find(key);
    if (!root.found) return store.open() || finish(store);
    if (!decode(db, root.value, now)) {
        if (read_status_ != read_status::missing) return false;
        root.action = db::update_action::erase;
        return store.open() || finish(store);
    }
    if (view_->header().kind != value_kind::zset) {
        read_status_ = read_status::wrong_type; return false;
    }
    if (view_->paged_payload()) return store.open() || finish(store);
    if (!import(store)) return false;
    if (!store.finish()) return finish(store);
    root.maintenance_only = true;
    return true;
}

bool paged_zset::run(shared_db& db, const std::string& key,
    const operation& callback) {
    reset();
    return result(db->transact_pages({key}, [&](page_transaction& tx) {
        zset_store store(tx, key);
        if (!open(db, tx, key, store, unix_time_ms())) return false;
        if (!callback(store)) {
            if (store.error() != zset_store::status::ok)
                read_status_ = map_read_status(store.error());
            return false;
        }
        return finish(store);
    }));
}

bool paged_zset::change(zset_store& store, const zset_member& value,
    const zadd_options& options, zadd_result& out) {
    zset_score current;
    if (!store.get(value.member, current)) return false;
    if ((current.found && options.nx) || (!current.found && options.xx))
        return true;
    const auto score = current.found && options.incr ?
        current.score + value.score : value.score;
    if (std::isnan(score)) {
        read_status_ = read_status::nan_score; return false;
    }
    if (current.found && ((options.gt && score <= current.score) ||
        (options.lt && score >= current.score))) return true;
    bool added, changed;
    if (!store.put(value.member, score, added, changed)) return false;
    out.added += added; out.changed += changed;
    out.processed = true; out.score = score;
    return true;
}

bool paged_zset::add(shared_db& db, const std::string& key,
    const std::vector<zset_member>& values, const zadd_options& options,
    zadd_result& out) {
    reset(); out = zadd_result();
    for (const auto& value : values) if (std::isnan(value.score)) {
        read_status_ = read_status::invalid_score; return false;
    }
    const bool ok = run(db, key, [&](zset_store& store) {
        for (const auto& value : values)
            if (!change(store, value, options, out)) return false;
        return true;
    });
    if (!ok) out = zadd_result();
    return ok;
}

bool paged_zset::remove(shared_db& db, const std::string& key,
    const std::vector<std::string>& values, long long& removed) {
    removed = 0;
    const bool ok = run(db, key, [&](zset_store& store) {
        for (const auto& member : values) {
            bool changed;
            if (!store.erase(member, changed)) return false;
            removed += changed;
        }
        return true;
    });
    if (!ok) removed = 0;
    return ok;
}

bool paged_zset::size(shared_db& db, const std::string& key,
    long long& count) {
    count = 0;
    const bool ok = run(db, key, [&](zset_store& store) {
        count = static_cast<long long>(store.metadata().elements);
        return true;
    });
    if (!ok) count = 0;
    return ok;
}

bool paged_zset::scores(shared_db& db, const std::string& key,
    const std::vector<std::string>& values, std::vector<zset_score>& out) {
    out.clear();
    const bool ok = run(db, key, [&](zset_store& store) {
        for (const auto& member : values) {
            zset_score score;
            if (!store.get(member, score)) return false;
            out.push_back(score);
        }
        return true;
    });
    if (!ok) out.clear();
    return ok;
}

bool paged_zset::rank(shared_db& db, const std::string& key,
    const std::string& member, bool reverse, zset_rank& out) {
    out = zset_rank();
    const bool ok = run(db, key, [&](zset_store& store) {
        if (!store.rank(member, out)) return false;
        if (out.found && reverse) out.rank = static_cast<long long>(
            store.metadata().elements) - out.rank - 1;
        return true;
    });
    if (!ok) out = zset_rank();
    return ok;
}

} // namespace dao
} // namespace pkv
