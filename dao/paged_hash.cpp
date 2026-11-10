#include "coder/compact_hash.h"
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
    if (root.value.compare(0, 3, "PMR") == 0)
        return store.open_live() || store_result(store);
    record_header header;
    bool native;
    if (!decode_metadata(db, root.value, header, native)) {
        if (read_status_ != read_status::missing) return false;
        root.action = db::update_action::erase;
        return store.open() || store_result(store);
    }
    if (header.kind != value_kind::hash) {
        read_status_ = read_status::wrong_type; return false;
    }
    if (native || view_->paged_payload())
        return store.open() || store_result(store);
    if (!import(store)) return false;
    if (!store.finish()) return store_result(store);
    root.maintenance_only = true;
    return true;
}

bool paged_hash::run_callback(shared_db& db, const std::string& key,
    const operation& callback, const packed_operation& packed,
    const promotion_operation& promote) {
    reset(); fields_.clear();
    bool promote_packed = false;
    auto edit_root = [&](db::batch_entry& root) {
        using choice = db::page_root_result;
        try {
            if (!root.found)
                return packed(root, nullptr) ? choice::ready : choice::fallback;
            if (!compact_hash::matches(root.value)) return choice::fallback;
            const compact_hash::view value(root.value);
            const auto deadline = value.deadline();
            if (deadline >= 0 && deadline <= unix_time_ms()) return choice::fallback;
            if (packed(root, &value)) return choice::ready;
            promote_packed = bool(promote);
            return choice::fallback;
        } catch (const std::runtime_error&) {
            read_status_ = read_status::invalid_record;
            return choice::rejected;
        }
    };
    auto apply = [&](page_transaction& tx) {
        if (promote_packed) {
            try {
                const compact_hash::view value(tx.roots().find(key)->value);
                map_store store(tx, key);
                if (!promote(store, value)) {
                    store_result(store);
                    return false;
                }
                return store.finish_hash() || store_result(store);
            } catch (const std::runtime_error&) {
                read_status_ = read_status::invalid_record;
                return false;
            }
        }
        map_store store(tx, key);
        if (!open(db, tx, key, store)) return false;
        if (!callback(store)) {
            if (store.error() != map_store::status::ok) store_result(store);
            return false;
        }
        return store.finish_hash() || store_result(store);
    };
    const db::page_root_callback edit(std::ref(edit_root));
    const auto result = scope_ ? scope_->transact(*db, key, std::ref(apply),
        packed ? &edit : nullptr) : packed ?
            db->transact_page(key, std::ref(apply), edit) :
            db->transact_page(key, std::ref(apply));
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
    map_store::hash_field_refs refs;
    refs.reserve(fields.size());
    for (const auto& field : fields) refs.emplace_back(&field.first, &field.second);
    return hmset(db, key, std::move(refs));
}

static bool put_hash_fields(map_store& store,
    const map_store::hash_field_refs::value_type* fields, size_t size, int& count) {
    for (size_t i = 0; i < size; ++i) {
        bool added;
        if (!store.put(*fields[i].first, *fields[i].second, false, added))
            return false;
        if (added) ++count;
    }
    return true;
}

static bool put_compact_fields(db::batch_entry& root, const compact_hash::view* packed,
    const map_store::hash_field_refs::value_type* fields, size_t size, int& count) {
    for (size_t i = 0; i < size; ++i)
        if (fields[i].first->size() > 254 || fields[i].second->size() > 254)
            return false;
    using field_view = std::pair<compact_hash::member_view, compact_hash::member_view>;
    field_view items[compact_hash::max_fields];
    size_t used = 0;
    if (packed) {
        for (unsigned i = 0; i < packed->count(); ++i) {
            items[used++] = packed->at(i);
        }
    }
    const auto previous = used;
    for (size_t i = 0; i < size; ++i) {
        const field_view field(*fields[i].first, *fields[i].second);
        auto* end = items + used;
        auto* position = std::lower_bound(items, end, field.first,
            [](const field_view& item, compact_hash::member_view key) {
                return item.first < key;
            });
        if (position != end && position->first.compare(field.first) == 0) {
            position->second = field.second;
            continue;
        }
        if (used == compact_hash::max_fields) return false;
        std::move_backward(position, end, end + 1);
        *position = field;
        ++used;
    }
    auto bytes = compact_hash::encode(items, items + used, used,
        packed ? packed->deadline() : -1);
    if (bytes.empty()) return false;
    count = static_cast<int>(used - previous);
    // HSET invalidates WATCH even when the field keeps its existing value.
    root.value = std::move(bytes);
    root.action = db::update_action::write;
    return true;
}

static bool put_compact_field(db::batch_entry& root, const compact_hash::view* packed,
    const std::string& name, const std::string& value, bool& added) {
    if (packed) {
        if (!packed->put(name, value, root.value, added)) return false;
        root.action = db::update_action::write;
        return true;
    }
    const map_store::hash_field_refs::value_type field(&name, &value);
    int count;
    if (!put_compact_fields(root, packed, &field, 1, count)) return false;
    added = count != 0;
    return true;
}

static bool promote_hash_fields(map_store& store, const compact_hash::view& packed,
    const map_store::hash_field_refs::value_type* fields, size_t size, int& count) {
    std::pair<std::string, std::string> previous[compact_hash::max_fields];
    map_store::hash_field_refs combined;
    combined.reserve(packed.count() + size);
    for (unsigned i = 0; i < packed.count(); ++i) {
        const auto field = packed.at(i);
        previous[i].first = field.first.str();
        previous[i].second = field.second.str();
        combined.emplace_back(&previous[i].first, &previous[i].second);
    }
    // build_hash retains the last value for duplicate fields.
    combined.insert(combined.end(), fields, fields + size);
    store.initialize(packed.deadline());
    if (store.error() != map_store::status::ok ||
        !store.build_hash_auto(combined, count)) return false;
    count -= static_cast<int>(packed.count());
    return true;
}

int paged_hash::hmset_pair(shared_db& db, const std::string& key,
    map_store::hash_field_refs::value_type first,
    map_store::hash_field_refs::value_type second) {
    const auto order = first.first->compare(*second.first);
    if (!order) {
        bool added;
        return hset(db, key, *second.first, *second.second, added) ? int(added) : -1;
    }
    if (order > 0) std::swap(first, second);
    const map_store::hash_field_refs::value_type fields[] = {first, second};
    int count = 0;
    auto tree = [&](map_store& store) {
        return put_hash_fields(store, fields, 2, count);
    };
    const bool ok = db->supports_compact_hashes() ?
        run_packed(db, key, tree,
            [&](db::batch_entry& root, const compact_hash::view* packed) {
                return put_compact_fields(root, packed, fields, 2, count);
            }, [&](map_store& store, const compact_hash::view& packed) {
                return promote_hash_fields(store, packed, fields, 2, count);
            }) : run(db, key, tree);
    return ok ? count : -1;
}

int paged_hash::hmset(shared_db& db, const std::string& key,
    map_store::hash_field_refs fields) {
    int count = 0;
    auto tree = [&](map_store& store) {
        if (fields.size() >= 64 && !store.exists())
            return store.build_hash_auto(fields, count);
        using field_ref = map_store::hash_field_refs::value_type;
        // Sort from the last request pair to the first, preserving that order
        // for equal fields so duplicate removal keeps the last supplied value.
        std::reverse(fields.begin(), fields.end());
        std::stable_sort(fields.begin(), fields.end(),
            [](const field_ref& a, const field_ref& b) { return *a.first < *b.first; });
        fields.erase(std::unique(fields.begin(), fields.end(),
            [](const field_ref& a, const field_ref& b) { return *a.first == *b.first; }),
            fields.end());
        return put_hash_fields(store, fields.data(), fields.size(), count);
    };
    const bool ok = db->supports_compact_hashes() && fields.size() <= compact_hash::max_fields ?
        run_packed(db, key, tree,
            [&](db::batch_entry& root, const compact_hash::view* packed) {
                return put_compact_fields(root, packed, fields.data(), fields.size(), count);
            }, [&](map_store& store, const compact_hash::view& packed) {
                return promote_hash_fields(store, packed, fields.data(), fields.size(), count);
            }) : run(db, key, tree);
    return ok ? count : -1;
}

bool paged_hash::hset(shared_db& db, const std::string& key,
    const std::string& name, const std::string& value) {
    bool added;
    return hset(db, key, name, value, added);
}

bool paged_hash::hset(shared_db& db, const std::string& key,
    const std::string& name, const std::string& value, bool& added) {
    added = false;
    bool inserted = false;
    auto tree = [&](map_store& store) {
        return store.put(name, value, false, inserted);
    };
    const bool ok = db->supports_compact_hashes() ?
        run_packed(db, key, tree,
            [&](db::batch_entry& root, const compact_hash::view* packed) {
                return put_compact_field(root, packed, name, value, inserted);
            }) : run(db, key, tree);
    if (ok) added = inserted;
    return ok;
}

bool paged_hash::hget(shared_db& db, const std::string& key,
    const std::string& name, std::string& value) {
    std::string result;
    bool found = false;
    auto tree = [&](map_store& store) {
        uint64_t bytes;
        return store.hash_field(name, &result, bytes, found);
    };
    if (!run_packed(db, key, tree, [&](db::batch_entry&, const compact_hash::view* packed) {
        found = packed && packed->get(name, result);
        return true;
    })) return false;
    if (!found) { read_status_ = read_status::missing; return false; }
    value.swap(result);
    return true;
}

int paged_hash::hdel(shared_db& db, const std::string& key,
    const std::vector<std::string>& names) {
    int count = 0;
    auto tree = [&](map_store& store) {
        for (const auto& name : names) {
            bool removed;
            if (!store.erase(name, removed)) return false;
            if (removed) ++count;
        }
        return true;
    };
    const bool ok = run_packed(db, key, tree,
        [&](db::batch_entry& root, const compact_hash::view* packed) {
            if (!packed) return true;
            uint64_t removed = 0;
            unsigned remaining = packed->count();
            for (const auto& name : names) {
                compact_hash::member_view value;
                unsigned index;
                if (!packed->lookup(name, value, &index)) continue;
                const auto bit = uint64_t{1} << index;
                if (removed & bit) continue;
                removed |= bit;
                if (!--remaining) break;
            }
            if (!removed) return true;
            if (remaining) {
                root.value = packed->without(removed, remaining);
                root.action = db::update_action::write;
            } else {
                root.action = db::update_action::erase;
            }
            count = static_cast<int>(packed->count() - remaining);
            return true;
        });
    return ok ? count : -1;
}

int paged_hash::hdel(shared_db& db, const std::string& key,
    const std::string& name) {
    return hdel(db, key, std::vector<std::string>{name});
}

bool paged_hash::hmget(shared_db& db, const std::string& key,
    const std::string* const* fields, size_t count, std::string* values,
    unsigned& found) {
    auto tree = [&](map_store& store) {
        return store.hash_fields(fields, count, values, found);
    };
    return run_packed(db, key, tree,
        [&](db::batch_entry&, const compact_hash::view* packed) {
            if (count > db::page_lock_scope::max_hash_batch) return false;
            found = 0;
            if (packed)
                for (size_t i = 0; i < count; ++i)
                    if (packed->get(*fields[i], values[i])) found |= 1U << i;
            return true;
        });
}

bool paged_hash::read_fields(shared_db& db, const std::string& key,
    const std::function<bool(const field_reader&)>& visit,
    size_t distinct, const map_store::key_filter& filter,
    const map_store::owning_visitor& collect, bool& scan_complete) {
    scan_complete = false;
    auto tree = [&](map_store& store) {
        const auto elements = store.metadata().elements;
        if (distinct >= 128 && elements >= 128 &&
            distinct >= elements - elements / 4) {
            scan_complete = true;
            if (!store.visit_all(collect, true, &filter)) return false;
            if (scan_complete) return true;
        }
        const auto read = [&](const std::string& field, std::string& value,
            bool& found) { return store.probe_hash_field(field, value, found); };
        return visit(std::cref(read));
    };
    bool completed = true;
    const bool ok = run_packed(db, key, tree,
        [&](db::batch_entry&, const compact_hash::view* packed) {
            const auto read = [&](const std::string& field, std::string& value,
                bool& found) {
                found = packed && packed->get(field, value);
                return true;
            };
            completed = visit(std::cref(read));
            return true;
        });
    return ok && completed;
}

bool paged_hash::hmget(shared_db& db, const std::string& key,
    const std::vector<std::string>& names,
    std::map<std::string, std::string>& result) {
    result.clear();
    const auto visit = [&](const field_reader& read) {
        for (const auto& name : names) {
            if (result.count(name)) continue;
            std::string value;
            bool found;
            if (!read(name, value, found)) return false;
            if (found) result.emplace(name, std::move(value));
        }
        return true;
    };
    bool scanned;
    const bool ok = read_fields(db, key, visit, 0, {}, {}, scanned);
    if (!ok) result.clear();
    return ok;
}

bool paged_hash::hgetall(shared_db& db, const std::string& key) {
    return hgetall(db, key, true);
}

bool paged_hash::hgetall(shared_db& db, const std::string& key,
    bool with_values) {
    auto tree = [&](map_store& store) {
        if (!store.visit([&](const std::string& name,
            const std::string& value) {
            fields_.emplace(name, value);
            return true;
        }, with_values)) return false;
        if (fields_.size() != store.metadata().elements) {
            read_status_ = read_status::invalid_record; return false;
        }
        return true;
    };
    return run_packed(db, key, tree,
        [&](db::batch_entry&, const compact_hash::view* packed) {
            if (packed)
                for (unsigned i = 0; i < packed->count(); ++i) {
                    const auto field = packed->at(i);
                    fields_.emplace(field.first.str(),
                        with_values ? field.second.str() : std::string());
                }
            return true;
        });
}

bool paged_hash::hgetall(shared_db& db, const std::string& key,
    bool with_values, field_list& out) {
    out.clear();
    auto tree = [&](map_store& store) {
        const auto count = store.metadata().elements;
        if (count > out.max_size()) {
            read_status_ = read_status::invalid_record;
            return false;
        }
        out.reserve(static_cast<size_t>(count));
        const auto collect = [&](std::string&& name, std::string&& value) {
            out.emplace_back(std::move(name), std::move(value));
            return true;
        };
        if (!store.visit_all(std::cref(collect), with_values)) return false;
        std::sort(out.begin(), out.end());
        if (out.size() == count && std::adjacent_find(out.begin(), out.end(),
            [](const field_list::value_type& a, const field_list::value_type& b) {
                return a.first == b.first;
            }) == out.end()) return true;
        read_status_ = read_status::invalid_record;
        return false;
    };
    const bool ok = run_packed(db, key, tree,
        [&](db::batch_entry&, const compact_hash::view* packed) {
            if (packed) {
                out.reserve(packed->count());
                for (unsigned i = 0; i < packed->count(); ++i) {
                    const auto field = packed->at(i);
                    out.emplace_back(field.first.str(),
                        with_values ? field.second.str() : std::string());
                }
            }
            return true;
        });
    if (!ok) out.clear();
    return ok;
}

bool paged_hash::hsetnx(shared_db& db, const std::string& key,
    const std::string& name, const std::string& value, bool& added) {
    added = false;
    auto tree = [&](map_store& store) {
        return store.put(name, value, true, added);
    };
    const bool ok = db->supports_compact_hashes() &&
        value.size() <= map_page_codec::max_payload_bytes ?
        run_packed(db, key, tree,
            [&](db::batch_entry& root, const compact_hash::view* packed) {
                compact_hash::member_view current;
                if (packed && packed->lookup(name, current)) return true;
                return put_compact_field(root, packed, name, value, added);
            }) : run(db, key, tree);
    if (!ok) added = false;
    return ok;
}

bool paged_hash::transform_field(shared_db& db, const std::string& key,
    const std::string& name, uint64_t old_limit,
    const map_store::hash_transform& transform) {
    const auto tree = [&](map_store& store) {
        return store.transform_hash_field(name, old_limit, transform);
    };
    if (!db->supports_compact_hashes()) return run(db, key, tree);
    bool transformed = true;
    const bool ok = run_packed(db, key, tree,
        [&](db::batch_entry& root, const compact_hash::view* packed) {
            compact_hash::member_view previous;
            const bool found = packed && packed->lookup(name, previous);
            const bool exceeded = found && previous.size() > old_limit;
            std::string old, next;
            if (found && !exceeded) old.assign(previous.data(), previous.size());
            transformed = transform(found ? &old : nullptr, exceeded, next);
            if (!transformed) return true;
            bool added;
            return put_compact_field(root, packed, name, next, added);
        });
    return ok && transformed;
}

bool paged_hash::hincrby(shared_db& db, const std::string& key,
    const std::string& name, long long delta, long long& value) {
    value = 0;
    auto increment_error = read_status::ok;
    const auto increment = [&](const std::string* bytes, bool, std::string& out) {
        long long old = 0;
        if (bytes && !parse_redis_integer(*bytes, old)) {
            increment_error = read_status::invalid_hash_integer;
            return false;
        }
        if (!checked_integer_add(old, delta, value)) {
            increment_error = read_status::integer_overflow;
            return false;
        }
        out = std::to_string(value);
        return true;
    };
    const bool ok = transform_field(db, key, name, UINT64_MAX,
        std::cref(increment));
    if (!ok || increment_error != read_status::ok) {
        if (increment_error != read_status::ok) read_status_ = increment_error;
        value = 0;
        return false;
    }
    return true;
}

bool paged_hash::hlen(shared_db& db, const std::string& key,
    long long& length) {
    length = 0;
    auto tree = [&](map_store& store) {
        length = static_cast<long long>(store.metadata().elements);
        return true;
    };
    const bool ok = run_packed(db, key, tree,
        [&](db::batch_entry&, const compact_hash::view* packed) {
            length = packed ? packed->count() : 0;
            return true;
        });
    if (!ok) length = 0;
    return ok;
}

bool paged_hash::hstrlen(shared_db& db, const std::string& key,
    const std::string& name, long long& length, bool& exists) {
    length = 0; exists = false;
    auto tree = [&](map_store& store) {
        uint64_t bytes;
        if (!store.hash_field(name, nullptr, bytes, exists)) return false;
        length = static_cast<long long>(bytes);
        return true;
    };
    const bool ok = run_packed(db, key, tree,
        [&](db::batch_entry&, const compact_hash::view* packed) {
            compact_hash::member_view value;
            exists = packed && packed->lookup(name, value);
            length = value.size();
            return true;
        });
    if (!ok) { length = 0; exists = false; }
    return ok;
}

} // namespace dao
} // namespace pkv
