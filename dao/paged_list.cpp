#include "coder/compact_list_root.h"
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
    record_header header;
    bool native;
    if (!decode_metadata(db, root.value, header, native, now)) {
        if (read_status_ != read_status::missing) return false;
        root.action = db::update_action::erase;
        return store.open() || store_result(store);
    }
    if (header.kind != value_kind::list) {
        read_status_ = read_status::wrong_type;
        return false;
    }
    if (native || view_->paged_payload())
        return store.open() || store_result(store);
    if (!import(store)) return false;
    if (!store.finish()) return store_result(store);
    root.maintenance_only = true;
    return true;
}

bool paged_list::run_callback(shared_db& db, const std::string& key,
    const operation& callback, const packed_operation& packed) {
    reset();
    const auto apply_root = [&](db::batch_entry& root) {
        using result = db::page_root_result;
        try {
            if (!root.found)
                return packed(root, nullptr) ? result::ready : result::fallback;
            if (compact_list_root::matches(root.value)) {
                const compact_list_root::view view(root.value);
                const auto deadline = view.deadline();
                if ((deadline < 0 || deadline > unix_time_ms()) &&
                    packed(root, &view)) return result::ready;
            }
        } catch (const std::runtime_error&) {
            read_status_ = read_status::invalid_record;
            return result::rejected;
        }
        return result::fallback;
    };
    const auto apply = [&](page_transaction& tx) {
        list_store store(tx, key);
        // decode_metadata reads the clock only for a stored deadline.
        if (!open(db, tx, key, store, -1)) return false;
        if (!callback(store)) {
            if (store.error() != list_store::status::ok) store_result(store);
            return false;
        }
        return store.finish() || store_result(store);
    };
    db::page_root_callback root;
    if (packed) root = std::cref(apply_root);
    const auto result = scope_ ?
        scope_->transact(*db, key, std::cref(apply), root ? &root : nullptr) :
        root ? db->transact_page(key, std::cref(apply), root) :
        db->transact_page(key, std::cref(apply));
    return batch_result(result);
}

bool paged_list::push(shared_db& db, const std::string& key,
    const std::vector<std::string>& values, bool left, bool only_existing,
    long long& length) {
    return push(db, key, list_values_view(values.data(), values.size()), left,
        only_existing, length);
}

bool paged_list::push(shared_db& db, const std::string& key,
    list_values_view values, bool left, bool only_existing, long long& length) {
    length = 0;
    const auto tree = [&](list_store& store) {
        if (only_existing && !store.exists()) return true;
        if (!store.push(values, left)) return false;
        length = static_cast<long long>(store.metadata().elements);
        return true;
    };
    const bool ok = db->supports_compact_lists() && !values.empty() && values.size() <= compact_list_root::max_members ?
        run_packed(db, key, tree,
            [&](db::batch_entry& root, const compact_list_root::view* packed) {
                if (!packed && only_existing) return true;
                const auto count = values.size() + (packed ? packed->count() : 0);
                if (packed) {
                    if (!packed->extend(values, left, root.value)) return false;
                } else {
                    auto bytes = compact_list_root::encode_push(values, left);
                    if (bytes.empty()) return false;
                    root.value = std::move(bytes);
                }
                root.action = db::update_action::write;
                length = static_cast<long long>(count);
                return true;
            }) : run(db, key, tree);
    if (!ok) length = 0;
    return ok;
}

static void finish_compact_pop(db::batch_entry& root,
    const compact_list_root::view& packed, unsigned count, bool left) {
    const auto remaining = packed.count() - count;
    if (!remaining) {
        root.action = db::update_action::erase;
        return;
    }
    root.value = packed.slice(left ? count : 0, remaining);
    root.action = db::update_action::write;
}

static void pop_compact_values(db::batch_entry& root,
    const compact_list_root::view& packed, bool left, long long count,
    std::vector<std::string>& values) {
    const auto wanted = static_cast<unsigned>(std::min<long long>(count, packed.count()));
    values.reserve(wanted);
    for (unsigned i = 0; i < wanted; ++i)
        values.push_back(packed.at(left ? i : packed.count() - 1 - i).str());
    finish_compact_pop(root, packed, wanted, left);
}

bool paged_list::pop_one(shared_db& db, const std::string& key, bool left,
    std::string& out, bool& found) {
    found = false;
    const auto tree = [&](list_store& store) {
        found = store.exists();
        return store.pop_one(left, out);
    };
    const bool ok = run_packed(db, key, tree,
        [&](db::batch_entry& root, const compact_list_root::view* packed) {
            found = packed != nullptr;
            if (packed) {
                out = packed->at(left ? 0 : packed->count() - 1).str();
                finish_compact_pop(root, *packed, 1, left);
            }
            return true;
        });
    if (!ok) { out.clear(); found = false; }
    return ok;
}

bool paged_list::pop(shared_db& db, const std::string& key, bool left,
    long long count, list_result& out) {
    out = list_result();
    if (count < 0) {
        reset(); read_status_ = read_status::invalid_integer; return false;
    }
    const auto tree = [&](list_store& store) {
        out.found = store.exists();
        return store.pop(static_cast<uint64_t>(count), left, &out.values);
    };
    const bool ok = run_packed(db, key, tree,
        [&](db::batch_entry& root, const compact_list_root::view* packed) {
            out.found = packed != nullptr;
            if (!packed || !count) return true;
            pop_compact_values(root, *packed, left, count, out.values);
            return true;
        });
    if (!ok) out = list_result();
    return ok;
}

bool paged_list::length(shared_db& db, const std::string& key, long long& out) {
    out = 0;
    const auto tree = [&](list_store& store) {
        out = static_cast<long long>(store.metadata().elements);
        return true;
    };
    const bool ok = run_packed(db, key, tree,
        [&](db::batch_entry&, const compact_list_root::view* packed) {
            out = packed ? packed->count() : 0;
            return true;
        });
    if (!ok) out = 0;
    return ok;
}

bool paged_list::range(shared_db& db, const std::string& key, long long start,
    long long end, std::vector<std::string>& out) {
    out.clear();
    const auto tree = [&](list_store& store) {
        return store.range(start, end, out);
    };
    const bool ok = run_packed(db, key, tree,
        [&](db::batch_entry&, const compact_list_root::view* packed) {
            size_t offset, count;
            if (packed && list_slice(packed->count(), start, end, offset, count)) {
                out.reserve(count);
                for (size_t i = 0; i < count; ++i)
                    out.push_back(packed->at(static_cast<unsigned>(offset + i)).str());
            }
            return true;
        });
    if (!ok) out.clear();
    return ok;
}

bool paged_list::index(shared_db& db, const std::string& key,
    const std::string& index_text, list_result& out) {
    out = list_result();
    const auto tree = [&](list_store& store) {
        if (!store.exists()) return true;
        long long position;
        if (!parse_redis_integer(index_text, position)) {
            read_status_ = read_status::invalid_integer; return false;
        }
        std::string value;
        if (!store.index(position, value, out.found)) return false;
        if (out.found) out.values.push_back(std::move(value));
        return true;
    };
    bool valid = true;
    const bool ok = run_packed(db, key, tree,
        [&](db::batch_entry&, const compact_list_root::view* packed) {
            if (!packed) return true;
            long long position;
            valid = parse_redis_integer(index_text, position);
            if (!valid) return true;
            if (position < 0) position += packed->count();
            if (position >= 0 && position < packed->count()) {
                out.found = true;
                out.values.push_back(packed->at(static_cast<unsigned>(position)).str());
            }
            return true;
        }) && valid;
    if (!valid) read_status_ = read_status::invalid_integer;
    if (!ok) out = list_result();
    return ok;
}

bool paged_list::set(shared_db& db, const std::string& key,
    const std::string& index_text, const std::string& value) {
    const auto tree = [&](list_store& store) {
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
    };
    auto error = read_status::ok;
    const bool ok = run_packed(db, key, tree,
        [&](db::batch_entry& root, const compact_list_root::view* packed) {
            if (!packed) {
                error = read_status::no_key;
                return true;
            }
            long long position;
            if (!parse_redis_integer(index_text, position)) {
                error = read_status::invalid_integer;
                return true;
            }
            size_t index;
            if (!list_index(packed->count(), position, index)) {
                error = read_status::index_range;
                return true;
            }
            std::string bytes;
            if (!packed->replace(static_cast<unsigned>(index), value, bytes)) return false;
            root.value = std::move(bytes);
            root.action = db::update_action::write;
            return true;
        });
    if (ok && error != read_status::ok) read_status_ = error;
    return ok && error == read_status::ok;
}

bool paged_list::trim(shared_db& db, const std::string& key, long long start,
    long long end) {
    const auto tree = [&](list_store& store) {
        return store.trim(start, end);
    };
    return run_packed(db, key, tree,
        [&](db::batch_entry& root, const compact_list_root::view* packed) {
            if (!packed) return true;
            size_t offset, count;
            if (!list_slice(packed->count(), start, end, offset, count)) {
                root.action = db::update_action::erase;
                return true;
            }
            if (count != packed->count())
                root.value = packed->slice(static_cast<unsigned>(offset),
                    static_cast<unsigned>(count));
            // LTRIM invalidates WATCH even when the full range is retained.
            root.action = db::update_action::write;
            return true;
        });
}

bool paged_list::insert(shared_db& db, const std::string& key, bool before,
    const std::string& pivot, const std::string& value, long long& length) {
    length = 0;
    const auto tree = [&](list_store& store) {
        return store.insert(before, pivot, value, length);
    };
    if (value.size() > list_store::max_member_bytes) return run(db, key, tree);
    const bool ok = run_packed(db, key, tree,
        [&](db::batch_entry& root, const compact_list_root::view* packed) {
            if (!packed) return true;
            unsigned index = 0;
            while (index < packed->count() && packed->at(index).compare(pivot) != 0)
                ++index;
            if (index == packed->count()) {
                length = -1;
                return true;
            }
            if (packed->count() == compact_list_root::max_members || value.size() > 255) return false;
            if (!before) ++index;
            std::vector<compact_list_root::member_view> items;
            items.reserve(packed->count() + 1);
            for (unsigned i = 0; i < index; ++i) items.push_back(packed->at(i));
            items.emplace_back(value);
            for (unsigned i = index; i < packed->count(); ++i) items.push_back(packed->at(i));
            auto bytes = compact_list_root::encode(items, packed->deadline());
            if (bytes.empty()) return false;
            root.value = std::move(bytes);
            root.action = db::update_action::write;
            length = static_cast<long long>(items.size());
            return true;
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
    const auto tree = [&](list_store& store) {
        return store.remove(count, value, removed);
    };
    const bool ok = run_packed(db, key, tree,
        [&](db::batch_entry& root, const compact_list_root::view* packed) {
            if (!packed) return true;
            const auto limit = count < 0 ? -count : count;
            const auto size = packed->count();
            uint64_t mask = 0;
            for (unsigned i = 0; i < size; ++i) {
                const auto index = count < 0 ? size - 1 - i : i;
                if (packed->at(index).compare(value) != 0) continue;
                mask |= uint64_t{1} << index;
                if (++removed == limit) break;
            }
            if (!removed) return true;
            if (removed == size) {
                root.action = db::update_action::erase;
            } else {
                root.value = packed->without(mask, size - static_cast<unsigned>(removed));
                root.action = db::update_action::write;
            }
            return true;
        });
    if (!ok) removed = 0;
    return ok;
}

bool paged_list::positions(shared_db& db, const std::string& key,
    const std::string& value, const list_search& options,
    std::vector<long long>& out) {
    out.clear();
    const auto tree = [&](list_store& store) {
        return store.positions(value, options, out);
    };
    bool valid = true;
    const bool ok = run_packed(db, key, tree,
        [&](db::batch_entry&, const compact_list_root::view* packed) {
            const auto matches = [&](size_t index) {
                return packed->at(static_cast<unsigned>(index)).compare(value) == 0;
            };
            valid = find_list_positions(packed ? packed->count() : 0,
                options, matches, out);
            return true;
        }) && valid;
    if (!valid) read_status_ = read_status::invalid_record;
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
        auto& source_root = *tx.roots().find(source);
        auto& target_root = *tx.roots().find(destination);
        bool checked_eligibility = false;
        if (source_root.found && compact_list_root::matches(source_root.value) &&
            (target_root.found ? compact_list_root::matches(target_root.value) :
                db->supports_compact_lists())) {
            try {
                const compact_list_root::view from(source_root.value);
                const auto source_deadline = from.deadline();
                if (source_deadline < 0 || source_deadline > now) {
                    if (eligible && !eligible(source)) return true;
                    checked_eligibility = true;
                    const auto index = from_left ? 0 : from.count() - 1;
                    const auto item = from.at(index);
                    const bool same = source == destination;
                    std::string encoded;
                    if (same && (from_left == to_left || from.count() == 1)) {
                        out.values.push_back(item.str());
                        out.found = true;
                        source_root.action = db::update_action::write;
                        return true;
                    }
                    std::vector<compact_list_root::member_view> items;
                    if (to_left) items.push_back(item);
                    if (same) {
                        for (unsigned i = 0; i < from.count(); ++i)
                            if (i != index) items.push_back(from.at(i));
                        if (!to_left) items.push_back(item);
                        encoded = compact_list_root::encode(items, source_deadline);
                    } else if (!target_root.found) {
                        if (!to_left) items.push_back(item);
                        encoded = compact_list_root::encode(items);
                    } else {
                        const compact_list_root::view to(target_root.value);
                        const auto deadline = to.deadline();
                        if ((deadline < 0 || deadline > now) && to.count() < compact_list_root::max_members) {
                            for (unsigned i = 0; i < to.count(); ++i)
                                items.push_back(to.at(i));
                            if (!to_left) items.push_back(item);
                            encoded = compact_list_root::encode(items, deadline);
                        }
                    }
                    if (!encoded.empty()) {
                        out.values.push_back(item.str());
                        if (!same) finish_compact_pop(source_root, from, 1, from_left);
                        target_root.value = std::move(encoded);
                        target_root.action = db::update_action::write;
                        out.found = true;
                        return true;
                    }
                }
            } catch (const std::runtime_error&) {
                read_status_ = read_status::invalid_record;
                return false;
            }
        }
        list_store from(tx, source);
        if (!open(db, tx, source, from, now)) return false;
        if (!from.exists()) return from.finish() || store_result(from);
        if (!checked_eligibility && eligible && !eligible(source)) return true;
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
            auto& root = *tx.roots().find(key);
            if (root.found && compact_list_root::matches(root.value)) {
                try {
                    const compact_list_root::view packed(root.value);
                    const auto deadline = packed.deadline();
                    if (deadline >= 0 && deadline <= now) {
                        root.action = db::update_action::erase;
                        continue;
                    }
                    if (eligible && !eligible(key)) continue;
                    selected_key = key;
                    pop_compact_values(root, packed, left, count, out.values);
                    out.found = true;
                    return true;
                } catch (const std::runtime_error&) {
                    read_status_ = read_status::invalid_record;
                    return false;
                }
            }
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
