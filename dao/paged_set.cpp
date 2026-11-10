#include "stdafx.h"
#include "paged_set.h"
#include "native_import.h"
#include "coder/compact_set.h"

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

bool paged_set::finish(map_store& store, bool pack_set) {
    if (store.finish(pack_set)) return true;
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
    if (root.value.compare(0, 3, "PMR") == 0)
        return store.open_live(now) || finish(store);
    record_header header;
    bool native;
    if (!decode_metadata(db, root.value, header, native, now)) {
        if (read_status_ != read_status::missing) return false;
        root.action = db::update_action::erase;
        return store.open() || finish(store);
    }
    if (header.kind != value_kind::set) {
        read_status_ = read_status::wrong_type; return false;
    }
    if (native || view_->paged_payload())
        return store.open() || finish(store);
    if (!import(store)) return false;
    if (!store.finish()) return finish(store);
    root.maintenance_only = true;
    return true;
}

bool paged_set::run_callback(shared_db& db, const std::string& key,
    const operation& callback, bool skip_missing,
    const packed_operation& packed, const promotion_operation& promote) {
    reset();
    auto apply = [&](page_transaction& tx) {
        if (packed) {
            auto& root = *tx.roots().find(key);
            try {
                if (!root.found && packed(root, nullptr)) return true;
                if (root.found && compact_set::matches(root.value)) {
                    const compact_set::view value(root.value);
                    const auto deadline = value.deadline();
                    if (deadline < 0 || deadline > unix_time_ms()) {
                        if (packed(root, &value)) return true;
                        if (promote) {
                            map_store store(tx, key, map_kind::set);
                            if (promote(store, value)) return finish(store);
                            read_status_ = map_read_status(store.error());
                            return false;
                        }
                    }
                }
            } catch (const std::runtime_error&) {
                read_status_ = read_status::invalid_record;
                return false;
            }
        }
        map_store store(tx, key, map_kind::set);
        // Opening reads the clock only for a stored deadline.
        if (!open(db, tx, key, store, -1)) return false;
        const auto previous_count = store.metadata().elements;
        if (!callback(store)) {
            if (store.error() != map_store::status::ok)
                read_status_ = map_read_status(store.error());
            return false;
        }
        return finish(store, store.metadata().elements < previous_count &&
            db->supports_compact_sets());
    };
    if (scope_) return result(scope_->transact(*db, key, std::ref(apply),
        nullptr, skip_missing));
    return result(skip_missing ? db->transact_existing_page(key, std::ref(apply)) :
        db->transact_page(key, std::ref(apply)));
}

bool paged_set::change(shared_db& db, const std::string& key,
    const std::vector<std::string>& members, bool add, long long& changed) {
    return change_members(db, key,
        [&](size_t i) -> const std::string& { return members[i]; },
        members.size(), add, changed);
}

bool paged_set::change_one(shared_db& db, const std::string& key,
    const std::string& member, bool add, long long& changed) {
    return change_members(db, key, [&](size_t) -> const std::string& {
        return member;
    }, 1, add, changed);
}

bool paged_set::change_members(shared_db& db, const std::string& key,
    const map_store::set_member_at& member_at, size_t count, bool add,
    long long& changed) {
    changed = 0;
    auto tree = [&](map_store& store) {
        if (!add) return store.erase_set(member_at, count, changed);
        if (count >= 128 && !store.exists())
            return store.build_set(member_at, count, changed);
        if (count >= 32 && store.metadata().grouped)
            return store.insert_grouped_set(member_at, count, changed);
        for (size_t i = 0; i < count; ++i) {
            const auto& member = member_at(i);
            bool updated;
            if (!store.put(member, "", true, updated)) return false;
            if (updated) ++changed;
        }
        return true;
    };
    auto compact = [&](db::batch_entry& root, const compact_set::view* packed) {
        if (!add) {
            if (!packed) return true;
            compact_set::erase_mask removed;
            unsigned remaining = packed->count();
            for (size_t i = 0; i < count && remaining; ++i) {
                const auto index = packed->find(member_at(i));
                if (index == packed->count()) continue;
                if (removed[index]) continue;
                removed.set(index);
                --remaining;
            }
            if (removed.none()) return true;
            if (remaining) {
                root.value = packed->without(removed, remaining);
                root.action = db::update_action::write;
            } else {
                root.action = db::update_action::erase;
            }
            changed = packed->count() - remaining;
            return true;
        }
        if (packed && count == 1) {
            std::string bytes;
            if (!packed->insert(member_at(0), bytes)) return false;
            if (!bytes.empty()) {
                root.value = std::move(bytes);
                root.action = db::update_action::write;
                changed = 1;
            }
            return true;
        }
        compact_set::member_view members[compact_set::max_members];
        size_t size = 0;
        int64_t deadline = -1;
        if (packed) {
            deadline = packed->deadline();
            for (unsigned i = 0; i < packed->count(); ++i)
                members[size++] = packed->at(i);
        }
        long long added = 0;
        for (size_t i = 0; i < count; ++i) {
            const compact_set::member_view member(member_at(i));
            if (member.size() > 255) return false;
            if (std::any_of(members, members + size,
                [&](compact_set::member_view value) { return !value.compare(member); }))
                continue;
            if (size == compact_set::max_members) return false;
            members[size++] = member;
            ++added;
        }
        if (!added) return true;
        auto bytes = compact_set::encode(members, size, deadline);
        if (bytes.empty()) return false;
        root.value = std::move(bytes);
        root.action = db::update_action::write;
        changed = added;
        return true;
    };
    const bool use_compact = !add ||
        (count && count <= compact_set::max_members && db->supports_compact_sets());
    auto promote = [&](map_store& store, const compact_set::view& packed) {
        std::string previous[compact_set::max_members];
        const auto existing = packed.count();
        for (unsigned i = 0; i < existing; ++i)
            previous[i] = packed.at(i).str();
        const auto combined = [&](size_t i) -> const std::string& {
            return i < existing ? previous[i] : member_at(i - existing);
        };
        store.initialize(packed.deadline());
        if (store.error() != map_store::status::ok ||
            !store.build_set(combined, existing + count, changed)) return false;
        changed -= existing;
        return true;
    };
    const bool ok = use_compact ?
        run_callback(db, key, std::ref(tree), false, std::ref(compact),
            add ? promotion_operation(std::ref(promote)) : promotion_operation()) :
        run(db, key, tree);
    if (!ok) changed = 0;
    return ok;
}

bool paged_set::size(shared_db& db, const std::string& key,
    long long& count) {
    count = 0;
    auto tree = [&](map_store& store) {
        count = static_cast<long long>(store.metadata().elements);
        return true;
    };
    const bool ok = run_callback(db, key, tree, false, [&](db::batch_entry&, const compact_set::view* packed) {
        if (packed) count = packed->count();
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
    const std::string* const* members, size_t count, unsigned& found) {
    auto tree = [&](map_store& store) {
        return store.set_membership(members, count, found);
    };
    auto compact = [&](db::batch_entry&, const compact_set::view* packed) {
        found = 0;
        if (!packed) return true;
        for (size_t i = 0; i < count; ++i)
            if (packed->contains(*members[i])) found |= 1U << i;
        return true;
    };
    return run_callback(db, key, std::ref(tree), false, std::ref(compact));
}

bool paged_set::membership(shared_db& db, const std::string& key,
    const std::vector<std::string>& members, std::vector<long long>& out) {
    return membership(db, key, [&](size_t i) -> const std::string& {
        return members[i];
    }, members.size(), out);
}

bool paged_set::membership(shared_db& db, const std::string& key,
    const map_store::set_member_at& member_at, size_t count,
    std::vector<long long>& out) {
    out.clear();
    auto tree = [&](map_store& store) {
        out.reserve(count);
        constexpr size_t limit = db::page_lock_scope::max_membership_batch;
        const std::string* members[limit];
        for (size_t offset = 0; offset < count;) {
            const auto size = std::min(limit, count - offset);
            for (size_t i = 0; i < size; ++i) members[i] = &member_at(offset + i);
            unsigned found;
            if (!store.set_membership(members, size, found, false)) return false;
            for (size_t i = 0; i < size; ++i) out.push_back((found >> i) & 1U);
            offset += size;
        }
        return true;
    };
    auto compact = [&](db::batch_entry&, const compact_set::view* packed) {
        out.reserve(count);
        for (size_t i = 0; i < count; ++i)
            out.push_back(packed && packed->contains(member_at(i)) ? 1 : 0);
        return true;
    };
    const bool ok = run_callback(db, key, std::ref(tree), false, std::ref(compact));
    if (!ok) out.clear();
    return ok;
}

bool paged_set::members(shared_db& db, const std::string& key,
    std::set<std::string>& out) {
    out.clear();
    auto tree = [&](map_store& store) {
        return store.visit([&](const std::string& member, const std::string&) {
            out.insert(member); return true;
        }, false);
    };
    const bool ok = run_callback(db, key, tree, false,
        [&](db::batch_entry&, const compact_set::view* packed) {
            if (packed)
                for (unsigned i = 0; i < packed->count(); ++i)
                    out.insert(packed->at(i).str());
            return true;
        });
    if (!ok) out.clear();
    return ok;
}

bool paged_set::visit_members(shared_db& db, const std::string& key,
    const member_visitor& visit, const packed_visitor& visit_packed) {
    auto tree = [&](map_store& store) { return visit_members(store, visit); };
    return run_callback(db, key, tree, false,
        [&](db::batch_entry&, const compact_set::view* packed) {
            if (packed && visit_packed) {
                visit_packed(*packed);
            } else if (packed) {
                const auto count = packed->count();
                for (unsigned i = 0; i < count; ++i)
                    visit(count, packed->at(i).str());
            }
            return true;
        });
}

bool paged_set::visit_members(map_store& store, const member_visitor& visit) {
    const auto count = store.metadata().elements;
    uint64_t seen = 0;
    size_t previous_bucket = 0;
    std::string previous;
    auto emit = [&](const std::string& member) {
        const auto bucket = scan_bucket(member);
        if (seen == count || (seen && (bucket < previous_bucket ||
            (bucket == previous_bucket && previous >= member)))) {
            read_status_ = read_status::invalid_record;
            return false;
        }
        visit(count, member);
        previous = member;
        previous_bucket = bucket;
        ++seen;
        return true;
    };
    if (!store.visit_all_set_members(std::ref(emit))) return false;
    if (seen == count) return true;
    read_status_ = read_status::invalid_record;
    return false;
}

bool paged_set::move(shared_db& db, const std::string& source,
    const std::string& destination, const std::string& member, bool& moved) {
    reset(); moved = false;
    const bool ok = result(db->transact_pages({source, destination},
        [&](page_transaction& tx) {
        const auto now = unix_time_ms();
        auto& source_root = *tx.roots().find(source);
        auto& target_root = *tx.roots().find(destination);
        if (source_root.found && compact_set::matches(source_root.value) &&
            (target_root.found ? compact_set::matches(target_root.value) :
                db->supports_compact_sets())) {
            try {
                const compact_set::view from(source_root.value);
                const auto deadline = from.deadline();
                if (deadline < 0 || deadline > now) {
                    const auto index = from.find(member);
                    if (source == destination) {
                        moved = index < from.count();
                        return true;
                    }
                    std::string inserted;
                    bool compatible = true;
                    if (target_root.found) {
                        const compact_set::view to(target_root.value);
                        const auto expiry = to.deadline();
                        compatible = (expiry < 0 || expiry > now) &&
                            (index == from.count() || to.insert(member, inserted));
                    } else if (index < from.count()) {
                        std::vector<std::string> members{member};
                        inserted = compact_set::encode(members);
                    }
                    if (compatible) {
                        if (index == from.count()) return true;
                        if (from.count() == 1) {
                            source_root.action = db::update_action::erase;
                        } else {
                            compact_set::erase_mask selected;
                            selected.set(index);
                            source_root.value = from.without(selected,
                                from.count() - 1);
                            source_root.action = db::update_action::write;
                        }
                        if (!inserted.empty()) {
                            target_root.value = std::move(inserted);
                            target_root.action = db::update_action::write;
                        }
                        moved = true;
                        return true;
                    }
                }
            } catch (const std::runtime_error&) {
                read_status_ = read_status::invalid_record;
                return false;
            }
        }
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
        return finish(from, moved && db->supports_compact_sets()) && finish(to);
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
