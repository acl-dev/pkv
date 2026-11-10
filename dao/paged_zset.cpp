#include "coder/compact_zset.h"
#include "stdafx.h"
#include "paged_zset.h"
#include "native_import.h"
#include <cmath>
#include <functional>

namespace pkv {
namespace dao {

namespace {
enum class zadd_decision { skip, apply, invalid };

zadd_decision update_score(const zadd_options& options, const zset_score& current,
    double input, double& score) {
    if ((current.found && options.nx) || (!current.found && options.xx))
        return zadd_decision::skip;
    score = current.found && options.incr ? current.score + input : input;
    if (std::isnan(score)) return zadd_decision::invalid;
    if (current.found && ((options.gt && score <= current.score) ||
        (options.lt && score >= current.score))) return zadd_decision::skip;
    return zadd_decision::apply;
}

template<class Member>
bool fold_bulk_members(const Member* values, size_t count,
    std::vector<zset_store::member_ref>& members, long long& changed) {
    if (count < 512 || count > UINT16_MAX) return false;
    size_t capacity = 1024;
    while (capacity < count * 2) capacity *= 2;
    std::vector<uint16_t> index(capacity);
    const auto mask = capacity - 1;
    const std::hash<std::string> hash;
    for (size_t i = 0; i < count; ++i) {
        const auto& value = values[i];
        auto slot = hash(value.member) & mask;
        unsigned probes = 0;
        while (index[slot] && *members[index[slot] - 1].member != value.member) {
            if (++probes == 16) {
                // Keep collision-heavy input bounded by the sorting path.
                members.clear();
                changed = 0;
                return false;
            }
            slot = (slot + 1) & mask;
        }
        if (!index[slot]) {
            members.push_back({&value.member, value.score});
            index[slot] = static_cast<uint16_t>(members.size());
            ++changed;
        } else {
            auto& existing = members[index[slot] - 1];
            if (existing.score != value.score) {
                existing.score = value.score;
                ++changed;
            }
        }
    }
    return true;
}
}

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
    if (root.value.compare(0, 3, "PZR") == 0)
        return store.open_live(now) || finish(store);
    record_header header;
    bool native;
    if (!decode_metadata(db, root.value, header, native, now)) {
        if (read_status_ != read_status::missing) return false;
        root.action = db::update_action::erase;
        return store.open() || finish(store);
    }
    if (header.kind != value_kind::zset) {
        read_status_ = read_status::wrong_type; return false;
    }
    if (native || view_->paged_payload())
        return store.open() || finish(store);
    if (!import(store)) return false;
    if (!store.finish()) return finish(store);
    root.maintenance_only = true;
    return true;
}

bool paged_zset::run_callback(shared_db& db, const std::string& key,
    const operation& callback, bool skip_missing,
    const packed_operation& packed_callback) {
    reset();
    auto apply = [&](page_transaction& tx) {
        if (packed_callback) {
            auto& root = *tx.roots().find(key);
            if (root.found && compact_zset::matches(root.value)) {
                try {
                    const compact_zset::view packed(root.value);
                    packed.validate_fast();
                    const auto deadline = packed.deadline();
                    if (deadline >= 0 && deadline <= unix_time_ms()) {
                        root.action = db::update_action::erase;
                        return true;
                    }
                    return packed_callback(packed, root);
                } catch (const std::runtime_error&) {
                    read_status_ = read_status::invalid_record;
                    return false;
                }
            }
        }
        zset_store store(tx, key);
        // Opening reads the clock only for a stored deadline.
        if (!open(db, tx, key, store, -1)) return false;
        if (!callback(store)) {
            if (store.error() != zset_store::status::ok)
                read_status_ = map_read_status(store.error());
            return false;
        }
        return finish(store);
    };
    if (scope_) return result(scope_->transact(*db, key, std::ref(apply),
        nullptr, skip_missing));
    return result(skip_missing ? db->transact_existing_page(key, std::ref(apply)) :
        db->transact_page(key, std::ref(apply)));
}

bool paged_zset::change(zset_store& store, const std::string& member, double input,
    const zadd_options& options, zadd_result& out) {
    if (options.incr && std::isfinite(input) &&
        !options.nx && !options.xx && !options.gt && !options.lt) {
        bool added, changed;
        if (!store.increment(member, input, added, changed)) return false;
        out.added += added; out.changed += changed;
        out.processed = true; out.score = input;
        return true;
    }
    zset_score current;
    // put() validates the old member and score indexes itself. Only options
    // that inspect the previous score need an additional read here.
    if ((options.nx || options.xx || options.gt || options.lt || options.incr)
        && !store.get(member, current)) return false;
    double score;
    const auto decision = update_score(options, current, input, score);
    if (decision == zadd_decision::skip) return true;
    if (decision == zadd_decision::invalid) {
        read_status_ = read_status::nan_score; return false;
    }
    bool added, changed;
    if (!store.put(member, score, added, changed)) return false;
    out.added += added; out.changed += changed;
    out.processed = true; out.score = score;
    return true;
}

bool paged_zset::add_one(shared_db& db, const std::string& key,
    const std::string& member, double score, const zadd_options& options,
    zadd_result& out) {
    if (db->supports_compact_zsets() && member.size() < 256) {
        const struct {
            const std::string& member;
            double score;
        } value{member, score};
        return add_values(db, key, &value, 1, options, out);
    }
    out = zadd_result();
    if (std::isnan(score)) {
        reset(); read_status_ = read_status::invalid_score; return false;
    }
    const bool ok = run(db, key, [&](zset_store& store) {
        return change(store, member, score, options, out);
    });
    if (!ok) out = zadd_result();
    return ok;
}

bool paged_zset::add(shared_db& db, const std::string& key,
    const std::vector<zset_member>& values, const zadd_options& options,
    zadd_result& out) {
    return add_values(db, key, values.data(), values.size(), options, out);
}

// Keep the bounded bulk scratch array off single-member and native-tree paths.
// The caller has already validated a live compact root before selecting it.
template<class Member>
#if defined(__GNUC__)
__attribute__((noinline))
#endif
bool paged_zset::add_compact_values(page_transaction& tx, const std::string& key,
    const Member* values, size_t count, const zadd_options& options,
    zadd_result& out, int64_t deadline, bool existing) {
    auto& root = *tx.roots().find(key);
    std::array<compact_zset::entry_view, compact_zset::max_members * 2> items;
    size_t size = 0;
    if (existing) {
        const compact_zset::view packed(root.value);
        for (unsigned i = 0; i < packed.count(); ++i) {
            const auto item = packed.at(i);
            items[size++] = {item.first, packed.score(item.second)};
        }
    }
    for (size_t i = 0; i < count; ++i) {
        const auto& value = values[i];
        const auto end = items.begin() + size;
        auto found = std::find_if(items.begin(), end,
            [&](const compact_zset::entry_view& item) {
                return item.first.compare(value.member) == 0;
            });
        zset_score current;
        current.found = found != end;
        if (current.found) current.score = found->second;
        double score;
        const auto decision = update_score(options, current,
            value.score, score);
        if (decision == zadd_decision::skip) continue;
        if (decision == zadd_decision::invalid) {
            read_status_ = read_status::nan_score;
            return false;
        }
        if (!current.found) {
            items[size++] = {value.member, score};
            ++out.added;
            ++out.changed;
        } else if (found->second != score) {
            found->second = score;
            ++out.changed;
        }
        out.processed = true;
        out.score = score;
    }
    if (!out.changed) return true;
    auto bytes = compact_zset::try_encode(items.data(), size, deadline);
    if (!bytes.empty()) {
        root.value = std::move(bytes);
        root.action = db::update_action::write;
        return true;
    }
    zset_store store(tx, key);
    std::vector<zset_member> members;
    members.reserve(size);
    for (size_t i = 0; i < size; ++i)
        members.push_back({items[i].first.str(), items[i].second});
    if (!store.build(members, deadline)) {
        read_status_ = map_read_status(store.error());
        return false;
    }
    return finish(store);
}

template<class Member>
bool paged_zset::add_values(shared_db& db, const std::string& key,
    const Member* values, size_t count, const zadd_options& options,
    zadd_result& out) {
    reset(); out = zadd_result();
    for (size_t i = 0; i < count; ++i) if (std::isnan(values[i].score)) {
        read_status_ = read_status::invalid_score; return false;
    }
    if (db->supports_compact_zsets() && count && count <= compact_zset::max_members &&
        std::all_of(values, values + count, [](const Member& value) {
            return value.member.size() < 256;
        })) {
        auto apply = [&](page_transaction& tx) {
            auto& root = *tx.roots().find(key);
            if (!root.found || compact_zset::matches(root.value)) {
                try {
                    int64_t deadline = -1;
                    bool existing = false;
                    if (root.found) {
                        const compact_zset::view packed(root.value);
                        deadline = packed.deadline();
                        if (deadline >= 0 && deadline <= unix_time_ms()) {
                            packed.validate_fast();
                            deadline = -1;
                        } else {
                            if (count == 1) {
                                const bool cached = packed.validate_fast(false);
                                const auto& value = values[0];
                                size_t offset;
                                unsigned insertion;
                                const auto index = packed.member_index(value.member,
                                    &offset, &insertion);
                                zset_score current;
                                current.found = index < packed.count();
                                if (current.found) current.score = packed.score(offset);
                                double score;
                                const auto decision = update_score(options, current,
                                    value.score, score);
                                if (decision == zadd_decision::skip) {
                                    if (!cached) packed.remember_validated();
                                    return true;
                                }
                                if (decision == zadd_decision::invalid) {
                                    read_status_ = read_status::nan_score;
                                    return false;
                                }
                                if (current.found && (current.score == score ||
                                    packed.replace_score(root.value, index, score))) {
                                    if (current.score == score && !cached)
                                        packed.remember_validated();
                                    out.processed = true;
                                    out.score = score;
                                    if (current.score != score) {
                                        out.changed = 1;
                                        root.action = db::update_action::write;
                                    }
                                    return true;
                                }
                                if (!current.found && packed.insert_member(
                                    value.member, score, insertion, root.value)) {
                                    out.processed = true;
                                    out.score = score;
                                    out.added = out.changed = 1;
                                    root.action = db::update_action::write;
                                    return true;
                                }
                            }
                            if (count != 1) packed.validate_fast();
                            existing = true;
                        }
                    }
                    return add_compact_values(tx, key, values, count,
                        options, out, deadline, existing);
                } catch (const std::runtime_error&) {
                    read_status_ = read_status::invalid_record;
                    return false;
                }
            }
            zset_store store(tx, key);
            if (!open(db, tx, key, store, -1)) return false;
            for (size_t i = 0; i < count; ++i) {
                const auto& value = values[i];
                if (!change(store, value.member, value.score, options, out)) return false;
            }
            return finish(store);
        };
        const bool ok = result(scope_ ? scope_->transact(*db, key, std::ref(apply)) :
            db->transact_page(key, std::ref(apply)));
        if (!ok) out = zadd_result();
        return ok;
    }
    const bool ok = run(db, key, [&](zset_store& store) {
        if (count >= 128 && !store.exists() && !options.nx && !options.xx &&
            !options.gt && !options.lt && !options.incr &&
            std::all_of(values, values + count, [](const Member& value) {
                return value.member.size() <= map_page_codec::max_payload_bytes;
            })) {
            std::vector<zset_store::member_ref> members;
            members.reserve(count);
            if (!fold_bulk_members(values, count, members, out.changed)) {
                std::vector<const Member*> ordered;
                ordered.reserve(count);
                for (size_t i = 0; i < count; ++i) ordered.push_back(values + i);
                std::sort(ordered.begin(), ordered.end(),
                    [](const Member* a, const Member* b) {
                        const auto order = a->member.compare(b->member);
                        // Both pointers refer into the original request array.
                        return order ? order < 0 : a < b;
                    });
                for (const auto* value : ordered) {
                    if (members.empty() || *members.back().member != value->member) {
                        members.push_back({&value->member, value->score});
                        ++out.changed;
                    } else if (members.back().score != value->score) {
                        members.back().score = value->score;
                        ++out.changed;
                    }
                }
            }
            out.added = members.size();
            out.processed = true;
            out.score = values[count - 1].score;
            return store.build_borrowed(members, -1);
        }
        for (size_t i = 0; i < count; ++i) {
            const auto& value = values[i];
            if (!change(store, value.member, value.score, options, out))
                return false;
        }
        return true;
    });
    if (!ok) out = zadd_result();
    return ok;
}

bool paged_zset::remove(shared_db& db, const std::string& key,
    const std::vector<std::string>& values, long long& removed) {
    removed = 0;
    auto tree = [&](zset_store& store) {
        for (const auto& member : values) {
            bool changed;
            if (!store.erase(member, changed)) return false;
            removed += changed;
        }
        return true;
    };
    auto compact = [&](const compact_zset::view& packed, db::batch_entry& root) {
        compact_zset::erase_mask erased;
        if (values.size() <= 4) {
            for (const auto& member : values) {
                const auto i = packed.member_index(member);
                if (i == packed.count()) continue;
                if (erased[i]) continue;
                erased.set(i);
                ++removed;
            }
            erase_compact(packed, root, erased, static_cast<unsigned>(removed));
            return true;
        }
        for (unsigned i = 0; i < packed.count(); ++i) {
            const auto member = packed.at(i).first;
            if (std::any_of(values.begin(), values.end(), [&](const std::string& value) {
                return member.compare(value) == 0;
            })) {
                erased.set(i);
                ++removed;
            }
        }
        erase_compact(packed, root, erased, static_cast<unsigned>(removed));
        return true;
    };
    const bool ok = run_callback(db, key, tree, false, compact);
    if (!ok) removed = 0;
    return ok;
}

void paged_zset::erase_compact(const compact_zset::view& packed,
    db::batch_entry& root, const compact_zset::erase_mask& erased, unsigned count) {
    if (!count) return;
    if (count == packed.count()) {
        root.action = db::update_action::erase;
        return;
    }
    root.value = packed.without(erased, packed.count() - count);
    root.action = db::update_action::write;
}

bool paged_zset::store_compact(page_transaction& tx, const std::string& key,
    std::vector<std::pair<std::string, double>> values, bool& stored) {
    auto bytes = compact_zset::try_encode(values);
    stored = !bytes.empty();
    if (!stored) return true;
    if (!tx.reset_pages(key)) {
        read_status_ = read_status::storage_error;
        return false;
    }
    auto& root = *tx.roots().find(key);
    root.value = std::move(bytes);
    root.action = db::update_action::write;
    root.maintenance_only = false;
    return true;
}

bool paged_zset::size(shared_db& db, const std::string& key,
    long long& count) {
    count = 0;
    auto tree = [&](zset_store& store) {
        count = static_cast<long long>(store.metadata().elements);
        return true;
    };
    auto compact = [&](const compact_zset::view& packed, db::batch_entry&) {
        count = packed.count();
        return true;
    };
    const bool ok = run_callback(db, key, tree, false, compact);
    if (!ok) count = 0;
    return ok;
}

bool paged_zset::score(shared_db& db, const std::string& key,
    const std::string& member, zset_score& out) {
    out = zset_score();
    auto tree = [&](zset_store& store) { return store.get(member, out); };
    auto compact = [&](const compact_zset::view& packed, db::batch_entry&) {
        out.found = packed.find(member, out.score);
        return true;
    };
    const bool ok = run_callback(db, key, tree, false, compact);
    if (!ok) out = zset_score();
    return ok;
}

bool paged_zset::scores(shared_db& db, const std::string& key,
    const std::string* const* members, size_t count, zset_score* out) {
    std::fill_n(out, count, zset_score{});
    auto tree = [&](zset_store& store) {
        return store.get_scores(members, count, out);
    };
    auto compact = [&](const compact_zset::view& packed, db::batch_entry&) {
        for (size_t i = 0; i < count; ++i)
            out[i].found = packed.find(*members[i], out[i].score);
        return true;
    };
    return run_callback(db, key, tree, false, compact);
}

bool paged_zset::scores(shared_db& db, const std::string& key,
    const std::vector<std::string>& values, std::vector<zset_score>& out) {
    out.resize(values.size());
    std::vector<const std::string*> members;
    members.reserve(values.size());
    for (const auto& value : values) members.push_back(&value);
    if (scores(db, key, members.data(), members.size(), out.data())) return true;
    out.clear();
    return false;
}

bool paged_zset::rank(shared_db& db, const std::string& key,
    const std::string& member, bool reverse, zset_rank& out) {
    out = zset_rank();
    auto tree = [&](zset_store& store) {
        if (!store.rank(member, out)) return false;
        if (out.found && reverse) out.rank = static_cast<long long>(
            store.metadata().elements) - out.rank - 1;
        return true;
    };
    auto compact = [&](const compact_zset::view& packed, db::batch_entry&) {
        unsigned rank = 0;
        out.found = packed.find(member, out.score, &rank);
        if (out.found) out.rank = reverse ? packed.count() - rank - 1 : rank;
        return true;
    };
    const bool ok = run_callback(db, key, tree, false, compact);
    if (!ok) out = zset_rank();
    return ok;
}

} // namespace dao
} // namespace pkv
