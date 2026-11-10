#include "stdafx.h"
#include "paged_zset.h"
#include "coder/compact_zset.h"

namespace pkv {
namespace dao {

namespace {

zset_member pop_compact_edge(const compact_zset::view& packed,
    db::batch_entry& root, bool maximum) {
    const auto rank = maximum ? packed.count() - 1 : 0;
    const auto index = packed.ordered_index(rank);
    const auto item = packed.at(index);
    zset_member value{item.first.str(), packed.score(item.second)};
    if (packed.count() == 1) root.action = db::update_action::erase;
    else {
        packed.erase_one(root.value, index, rank);
        root.action = db::update_action::write;
    }
    return value;
}

template<class Visitor>
unsigned visit_compact_range(const compact_zset::view& packed,
    const zset_query& query, Visitor visit) {
    const size_t size = packed.count();
    size_t first = 0, length = size;
    if (query.mode == zrange_mode::rank) {
        if (!list_slice(size, query.start, query.end, first, length))
            return 0;
    } else if (query.offset < 0 || query.limit == 0) {
        return 0;
    }
    auto skip = query.mode == zrange_mode::rank ? 0 : query.offset;
    const auto limit = query.mode == zrange_mode::rank ? -1 : query.limit;
    unsigned count = 0;
    for (size_t i = first; i < first + length; ++i) {
        const auto rank = query.reverse ? size - 1 - i : i;
        const auto index = query.mode == zrange_mode::lex ?
            static_cast<unsigned>(rank) : packed.ordered_index(rank);
        const auto item = packed.at(index);
        const auto score = packed.score(item.second);
        if (query.mode == zrange_mode::score && !zscore_matches(score, query))
            continue;
        if (query.mode == zrange_mode::lex &&
            !zlex_matches(item.first.str(), query)) continue;
        if (skip) { --skip; continue; }
        visit(index, item.first, score, length);
        if (++count == limit) break;
    }
    return count;
}

} // namespace

bool paged_zset::query_range(shared_db& db, const std::string& key,
    const zset_query& query, std::vector<zset_member>* out, long long& count) {
    count = 0;
    if (out) out->clear();
    auto tree = [&](zset_store& store) {
        return out ? store.range(query, *out) : store.count(query, count);
    };
    auto compact = [&](const compact_zset::view& packed, db::batch_entry&) {
        count = visit_compact_range(packed, query, [&](unsigned,
            compact_zset::member_view member, double score, size_t length) {
            if (!out) return;
            if (query.mode == zrange_mode::rank && out->empty())
                out->reserve(length);
            out->push_back({member.str(), score});
        });
        return true;
    };
    const bool ok = run_callback(db, key, tree, false, compact);
    if (!ok) {
        if (out) out->clear();
        count = 0;
    }
    return ok;
}

bool paged_zset::select(shared_db& db, const std::string& key,
    const zset_query& query, std::vector<zset_member>& out) {
    long long ignored;
    return query_range(db, key, query, &out, ignored);
}

bool paged_zset::range(shared_db& db, const std::string& key, long long start,
    long long end, bool reverse, std::vector<zset_member>& out) {
    zset_query query;
    query.start = start; query.end = end; query.reverse = reverse;
    return select(db, key, query, out);
}

bool paged_zset::count(shared_db& db, const std::string& key,
    const zset_query& query, long long& out) {
    return query_range(db, key, query, nullptr, out);
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
    auto tree = [&](zset_store& store) {
        std::vector<zset_member> values;
        if (!store.range(query, values) || !erase(store, values)) return false;
        removed = static_cast<long long>(values.size());
        return true;
    };
    auto compact = [&](const compact_zset::view& packed, db::batch_entry& root) {
        compact_zset::erase_mask erased;
        const auto count = visit_compact_range(packed, query, [&](unsigned index,
            compact_zset::member_view, double, size_t) {
            erased.set(index);
        });
        erase_compact(packed, root, erased, count);
        removed = count;
        return true;
    };
    const bool ok = run_callback(db, key, tree, false, compact);
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
        const auto now = unix_time_ms();
        const auto& root = *tx.roots().find(source);
        if (root.found && compact_zset::matches(root.value)) {
            try {
                const compact_zset::view packed(root.value);
                packed.validate_fast();
                const auto deadline = packed.deadline();
                if (deadline < 0 || deadline > now) {
                    visit_compact_range(packed, query, [&](unsigned,
                        compact_zset::member_view member, double score, size_t) {
                        values.push_back({member.str(), score});
                    });
                }
            } catch (const std::runtime_error&) {
                read_status_ = read_status::invalid_record;
                return false;
            }
        } else {
            zset_store from(tx, source);
            if (!open(db, tx, source, from, now)) return false;
            if (!from.range(query, values)) {
                read_status_ = map_read_status(from.error()); return false;
            }
            if (!finish(from)) return false;
        }
        stored = static_cast<long long>(values.size());
        if (db->supports_compact_zsets() && !values.empty() && values.size() <= compact_zset::max_members) {
            std::vector<compact_zset::entry> items;
            items.reserve(values.size());
            for (const auto& value : values) items.emplace_back(value.member, value.score);
            bool compact;
            if (!store_compact(tx, destination, std::move(items), compact)) return false;
            if (compact) return true;
        }
        zset_store target(tx, destination);
        if (values.size() >= 128) {
            const bool built = target.build(values, -1);
            return finish(target) && built;
        }
        target.initialize();
        for (const auto& value : values) {
            bool added, changed;
            if (!target.put(value.member, value.score, added, changed))
                return finish(target);
        }
        return finish(target);
    }));
    if (!ok) stored = 0;
    return ok;
}

bool paged_zset::pop(zset_store& store, long long count, bool maximum,
    std::vector<zset_member>& out) {
    const auto size = store.metadata().elements;
    const auto wanted = std::min<uint64_t>(count, size);
    const auto remaining = size - wanted;
    // Rebuild only a small remainder, with at least as many removals as
    // survivors. Smaller pops retain the edge-removal path.
    if ((wanted > 1 && !remaining) ||
        (wanted >= 64 && remaining <= 128 && wanted >= remaining))
        return store.pop_rebuild(wanted, maximum, out, true);
    return store.pop_edges(wanted, maximum, out);
}

void paged_zset::pop_compact(const compact_zset::view& packed,
    db::batch_entry& root, long long count, bool maximum,
    std::vector<zset_member>& out) {
    const auto wanted = static_cast<unsigned>(std::min<long long>(count,
        packed.count()));
    out.reserve(wanted);
    if (wanted == 1) {
        out.push_back(pop_compact_edge(packed, root, maximum));
        return;
    }
    compact_zset::erase_mask erased;
    for (unsigned i = 0; i < wanted; ++i) {
        const auto index = packed.ordered_index(maximum ? packed.count() - 1 - i : i);
        const auto item = packed.at(index);
        out.push_back({item.first.str(), packed.score(item.second)});
        erased.set(index);
    }
    erase_compact(packed, root, erased, wanted);
}

bool paged_zset::pop(shared_db& db, const std::string& key, long long count,
    bool maximum, std::vector<zset_member>& out) {
    reset(); out.clear();
    if (count < 0) {
        read_status_ = read_status::invalid_integer; return false;
    }
    auto tree = [&](zset_store& store) {
        return pop(store, count, maximum, out);
    };
    auto compact = [&](const compact_zset::view& packed, db::batch_entry& root) {
        pop_compact(packed, root, count, maximum, out);
        return true;
    };
    const bool ok = run_callback(db, key, tree, true, compact);
    if (!ok) out.clear();
    return ok;
}

bool paged_zset::pop_one(shared_db& db, const std::string& key, bool maximum,
    zset_member& out, bool& found) {
    found = false;
    auto tree = [&](zset_store& store) {
        if (!store.exists()) return true;
        found = store.pop_edge(maximum, out);
        return found;
    };
    auto compact = [&](const compact_zset::view& packed, db::batch_entry& root) {
        out = pop_compact_edge(packed, root, maximum);
        found = true;
        return true;
    };
    const bool ok = run_callback(db, key, tree, true, compact);
    if (!ok) found = false;
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
            auto& root = *tx.roots().find(key);
            if (root.found && root.action != db::update_action::erase &&
                compact_zset::matches(root.value)) {
                try {
                    const compact_zset::view packed(root.value);
                    packed.validate_fast();
                    const auto deadline = packed.deadline();
                    if (deadline >= 0 && deadline <= now) {
                        root.action = db::update_action::erase;
                        continue;
                    }
                    pop_compact(packed, root, count, maximum, out.values);
                    selected_key = key; out.found = true;
                    return true;
                } catch (const std::runtime_error&) {
                    read_status_ = read_status::invalid_record;
                    return false;
                }
            }
            zset_store store(tx, key);
            if (!open(db, tx, key, store, now)) return false;
            if (!store.exists()) {
                if (!finish(store)) return false;
                continue;
            }
            if (!pop(store, count, maximum, out.values)) {
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
