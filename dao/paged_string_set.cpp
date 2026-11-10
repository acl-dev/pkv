#include "stdafx.h"
#include "paged_string.h"
#include "string_comparison.h"

namespace pkv {
namespace dao {

bool paged_string::matches(page_transaction& tx, const std::string& key,
    const std::string& expected, bool& equal) {
    if (view_->header().kind != value_kind::string) {
        read_status_ = read_status::wrong_type; return false;
    }
    if (!view_->paged_payload()) {
        if (record_string_equal(*view_, expected, equal)) return true;
        read_status_ = read_status::invalid_record; return false;
    }
    string_store store(tx, key);
    if (!store.open()) return stored(store);
    if (string_equal_chunks(store.metadata().length, expected,
        [&](long long first, long long last, std::string& out) {
            return store.range(first, last, out);
        }, equal)) return true;
    if (store.error() != string_store::status::ok) return stored(store);
    read_status_ = read_status::invalid_record; return false;
}
bool paged_string::current(shared_db& db, page_transaction& tx,
    const std::string& key, std::string& out) {
    if (view_->header().kind != value_kind::string) {
        read_status_ = read_status::wrong_type; return false;
    }
    if (!view_->paged_payload()) {
        if (view_->string_value(out)) return true;
        read_status_ = read_status::invalid_record; return false;
    }
    string_store store(tx, key);
    return (store.open() && store.read(out)) || stored(store);
}
bool paged_string::set(shared_db& db, const std::string& key,
    const char* data) { return set(db, key, std::string(data)); }
bool paged_string::set(shared_db& db, const std::string& key,
    const std::string& data) {
    set_result out;
    return set(db, key, data, set_options(), out);
}
bool paged_string::write_inline_root(db::batch_entry& entry,
    const std::string& value, int64_t expiry, bool compact) {
    if (string_page_codec::encode_inline(expiry, value, entry.value, compact) !=
        string_page_status::ok) {
        read_status_ = read_status::storage_error;
        return false;
    }
    entry.action = db::update_action::write;
    entry.clear_pages = true;
    entry.maintenance_only = false;
    return true;
}

bool paged_string::set(shared_db& db, const std::string& key,
    const std::string& data, const set_options& options, set_result& out) {
    reset(); out = set_result();
    const bool compare = options.compares();
    if (compare && !options.comparison) {
        read_status_ = read_status::unsupported_operation; return false;
    }
    const bool inspect = options.when != set_options::any ||
        options.keep_ttl || options.return_old;
    const bool inline_value = data.size() <= string_page_codec::inline_bytes;
    auto write_inline = [&](db::batch_entry& entry, int64_t expiry) {
        out.applied = write_inline_root(entry, data, expiry, db->supports_compact_strings());
        return out.applied;
    };
    auto apply = [&](page_transaction& tx) {
        auto& entry = *tx.roots().find(key);
        bool found = entry.found;
        int64_t expiry = options.expires_at_ms;
        const auto now = (inspect && found) || expiry >= 0 ?
            unix_time_ms() : int64_t(0);
        if (inspect && found && !decode(db, entry.value, now)) {
            if (read_status_ != read_status::missing) return false;
            found = false; entry.action = db::update_action::erase;
        }
        if (options.return_old && found) {
            if (!current(db, tx, key, out.old_value)) return false;
            out.had_old = true;
        }
        if ((options.when == set_options::only_missing && found) ||
            (options.needs_existing() && !found))
            return true;
        if (compare && found) {
            bool equal;
            if (!comparison(tx, key, options,
                options.return_old ? &out.old_value : nullptr, equal))
                return false;
            if (equal != options.equality()) return true;
        }
        if (options.keep_ttl && found) expiry = view_->header().expires_at_ms;
        if (inline_value && expiry >= -1 &&
            (expiry < 0 || expiry > now)) {
            if (!tx.reset_pages(key)) {
                read_status_ = read_status::storage_error;
                return false;
            }
            return write_inline(entry, expiry);
        }
        string_store store(tx, key);
        if (expiry >= 0 && expiry <= now) {
            store.initialize();
            if (!store.clear()) return stored(store);
        } else if (!store.assign(data) || !store.set_expiry(expiry))
            return stored(store);
        if (!store.finish()) return stored(store);
        out.applied = true;
        return true;
    };
    auto root = [&](db::batch_entry& entry) {
        return write_inline(entry, -1) ? db::page_root_result::ready :
            db::page_root_result::rejected;
    };
    db::batch_status status;
    if (inline_value && !inspect && options.expires_at_ms == -1) {
        status = scope_ ? scope_->replace_root(*db, key, std::ref(root)) :
            db->replace_page_root(key, std::ref(root));
    } else {
        status = scope_ ? scope_->transact(*db, key, std::ref(apply)) :
            db->transact_page(key, std::ref(apply));
    }
    if (result(status)) return true;
    out = set_result(); return false;
}

bool paged_string::mget(shared_db& db, const std::vector<std::string>& keys,
    std::vector<string_result>& values) {
    reset(); values.clear();
    auto apply = [&](page_transaction& tx) {
        const auto now = unix_time_ms();
        const auto roots = tx.roots();
        values.resize(keys.size());
        size_t local_slots[64];
        std::vector<size_t> large_slots;
        auto* slots = local_slots;
        if (roots.size() > 64) {
            large_slots.resize(roots.size());
            slots = large_slots.data();
        }
        std::fill_n(slots, roots.size(), keys.size());
        for (size_t i = 0; i < keys.size(); ++i) {
            auto& entry = *roots.find(keys[i]);
            auto& first = slots[&entry - roots.begin()];
            auto& value = values[i];
            if (first != keys.size()) {
                value = values[first];
                continue;
            }
            first = i;
            if (!entry.found) continue;
            if (entry.value.compare(0, 3, "PSR") == 0) {
                string_root_header header;
                if (string_page_codec::decode_header(entry.value, header) ==
                    string_page_status::ok) {
                    if (header.expires_at_ms >= 0 && header.expires_at_ms <= now)
                        continue;
                    if (!header.pages) {
                        if (roots.size() > 1) {
                            // The unmodified root action discards this snapshot
                            // after the read. Reuse its allocation for the value.
                            value.value.swap(entry.value);
                            value.value.erase(0, header.payload_offset);
                        } else {
                            // Let the single-root workspace retain its buffer.
                            value.value.assign(entry.value.data() + header.payload_offset,
                                static_cast<size_t>(header.length));
                        }
                        value.found = true;
                        continue;
                    }
                }
            }
            if (decode(db, entry.value, now)) {
                if (view_->header().kind == value_kind::string) {
                    if (!current(db, tx, entry.key, value.value)) return false;
                    value.found = true;
                }
            } else if (read_status_ != read_status::missing)
                return false;
        }
        return true;
    };
    const auto status = db->transact_pages(keys, std::ref(apply));
    if (result(status)) return true;
    values.clear(); return false;
}
bool paged_string::mset(shared_db& db,
    const std::map<std::string, std::string>& values, bool only_missing,
    bool& applied) {
    reset(); applied = false;
    std::vector<std::string> keys;
    keys.reserve(values.size());
    for (const auto& value : values) {
        if (value.second.size() > redis_string_max) {
            read_status_ = read_status::string_too_large; return false;
        }
        keys.push_back(value.first);
    }
    auto apply = [&](page_transaction& tx) {
        if (only_missing) {
            const auto now = unix_time_ms();
            for (const auto& entry : tx.roots()) {
                if (!entry.found) continue;
                if (decode(db, entry.value, now)) return true;
                if (read_status_ != read_status::missing) return false;
            }
        }
        // The unique input keys and transaction roots use the same order.
        auto value = values.begin();
        for (auto& entry : tx.roots()) {
            const auto& data = (value++)->second;
            if (data.size() <= string_page_codec::inline_bytes) {
                // This root has no staged pages yet. Clear the old family
                // atomically when the new inline root is published.
                if (!write_inline_root(entry, data, -1, db->supports_compact_strings()))
                    return false;
                continue;
            }
            string_store store(tx, entry.key);
            if (!store.assign(data) || !store.finish())
                return stored(store);
        }
        applied = true;
        return true;
    };
    const auto status = db->transact_pages(std::move(keys), std::ref(apply));
    if (result(status)) return true;
    applied = false; return false;
}
} // namespace dao
} // namespace pkv
