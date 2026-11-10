#include "coder/packed_hash.h"
#include "coder/compact_zset.h"
#include "coder/compact_set.h"
#include "coder/compact_hash.h"
#include "coder/compact_list_root.h"
#include "stdafx.h"
#include <climits>
#include "json_key.h"
#include "coder/zset_page.h"
#include "coder/list_page.h"
#include "coder/map_page.h"
#include "coder/string_page.h"
#include "db/stream_store.h"

namespace pkv {
namespace dao {

bool json_key::copy(shared_db& source_db, const std::string& source,
    shared_db& destination_db, const std::string& destination, bool replace,
    bool& copied) {
    reset(); copied = false;
    if (source_db == destination_db && source == destination) {
        read_status_ = read_status::same_object;
        return false;
    }
    for (;;) {
        bool paged = false, legacy = false;
        copied = false;
        const auto status = source_db->copy_to(source, *destination_db,
            destination, [&](bool found, const std::string& from,
                bool target_found, std::string& to, db::update_action& action) {
            const auto now = unix_time_ms();
            if (!found) return true;
            if (!decode(source_db, from, now))
                return read_status_ == read_status::missing;
            legacy = !view_->paged_payload();
            if (!legacy || (view_->native_importable() &&
                source_db->supports_native_pages() &&
                destination_db->allows_page_writes())) {
                paged = true; return false;
            }
            if (!view_->valid_payload()) {
                read_status_ = read_status::invalid_record;
                return false;
            }
            auto source_view = std::move(view_);
            if (target_found && !replace) {
                if (decode(destination_db, to, now)) return true;
                if (read_status_ != read_status::missing) return false;
            }
            view_ = std::move(source_view);
            if (&source_db->value_codec() == &destination_db->value_codec()) {
                to = from;
            } else {
                value_record record;
                if (!materialize(record) || !encode(destination_db, record, to))
                    return false;
            }
            action = db::update_action::write;
            copied = true;
            return true;
        });
        if (paged) {
            bool retry = false;
            if (!copy_paged(source_db, source, destination_db, destination,
                replace, copied, retry)) return false;
            if (retry) continue;
            return true;
        }
        if (!page_result(status)) { copied = false; return false; }
        return true;
    }
}

bool json_key::rename(shared_db& db, const std::string& source,
    const std::string& destination, bool nx, bool& renamed) {
    reset(); renamed = false;
    for (;;) {
        bool paged = false, legacy = false;
        renamed = false;
        const auto status = db->transact({source, destination},
            [&](db::batch_view entries) {
            const auto now = unix_time_ms();
            auto& from = *entries.find(source);
            if (!from.found) {
                read_status_ = read_status::no_key; return false;
            }
            if (!decode(db, from.value, now)) {
                if (read_status_ == read_status::missing)
                    read_status_ = read_status::no_key;
                return false;
            }
            legacy = !view_->paged_payload();
            if (!legacy || (view_->native_importable() &&
                db->allows_page_writes())) {
                paged = true; return false;
            }
            if (!view_->valid_payload()) {
                read_status_ = read_status::invalid_record; return false;
            }
            if (source == destination) { renamed = !nx; return true; }
            auto& to = *entries.find(destination);
            if (nx && to.found) {
                if (decode(db, to.value, now)) return true;
                if (read_status_ != read_status::missing) return false;
            }
            // Preserve legacy/read-only encoding and exact expiry.
            to.value = from.value;
            to.action = db::update_action::write;
            from.action = db::update_action::erase;
            renamed = true;
            return true;
        });
        if (paged) {
            bool retry = false;
            if (!rename_paged(db, source, destination, nx, renamed, retry))
                return false;
            if (retry) continue;
            return true;
        }
        if (!page_result(status)) { renamed = false; return false; }
        return true;
    }
}

bool json_key::del(shared_db& db, const std::string& key) {
    long long removed;
    return del_one(db, key, removed) && removed != 0;
}

bool json_key::del_one(shared_db& db, const std::string& key, long long& removed) {
    removed = 0;
    bool valid = true;
    reset();
    const bool ok = db->update(key, [&](bool found, std::string& bytes) {
        if (!found) return db::update_action::keep;
        record_header header;
        bool native;
        if (!decode_metadata(db, bytes, header, native)) {
            valid = read_status_ == read_status::missing;
            return valid ? db::update_action::erase : db::update_action::keep;
        }
        removed = 1;
        return db::update_action::erase;
    });
    if (!ok) {
        removed = 0;
        read_status_ = read_status::storage_error;
    }
    return ok && valid;
}

bool json_key::del(shared_db& db, const std::vector<std::string>& keys, long long& removed) {
    reset();
    removed = 0;
    const auto status = db->transact(keys, [&](db::batch_view entries) {
        const auto now = unix_time_ms();
        for (auto& entry : entries) {
            if (!entry.found) continue;
            if (decode(db, entry.value, now)) ++removed;
            else if (read_status_ != read_status::missing) return false;
            entry.action = db::update_action::erase;
        }
        return true;
    });
    if (status == db::batch_status::error) read_status_ = read_status::storage_error;
    else if (status == db::batch_status::unsupported) read_status_ = read_status::unsupported_operation;
    if (status != db::batch_status::ok) removed = 0;
    return status == db::batch_status::ok;
}

bool json_key::exists(shared_db& db, const std::vector<std::string>& keys, long long& count) {
    reset();
    count = 0;
    const auto status = db->transact(keys, [&](db::batch_view entries) {
        const auto now = unix_time_ms();
        std::unordered_map<std::string, bool> alive;
        for (const auto& entry : entries) {
            bool found = false;
            if (entry.found) {
                found = decode(db, entry.value, now);
                if (!found && read_status_ != read_status::missing) return false;
            }
            alive.emplace(entry.key, found);
        }
        // EXISTS counts duplicate arguments separately.
        for (const auto& key : keys) count += alive.at(key);
        return true;
    });
    if (status == db::batch_status::error) read_status_ = read_status::storage_error;
    else if (status == db::batch_status::unsupported) read_status_ = read_status::unsupported_operation;
    if (status != db::batch_status::ok) count = 0;
    return status == db::batch_status::ok;
}

bool json_key::type(shared_db& db, const std::string& key, std::string& out) {
    if (!read(db, key)) return false;
    switch (view_->header().kind) {
    case value_kind::string: out = "string"; break;
    case value_kind::hash: out = "hash"; break;
    case value_kind::list: out = "list"; break;
    case value_kind::set: out = "set"; break;
    case value_kind::zset: out = "zset"; break;
    case value_kind::stream: out = "stream"; break;
    }
    return true;
}

bool json_key::expire(shared_db& db, const std::string& key, long long seconds) {
    const long long now = unix_time_ms();
    if (seconds > (LLONG_MAX - now) / 1000 || seconds < LLONG_MIN / 1000) {
        read_status_ = read_status::storage_error;
        return false;
    }
    return set_expiry(db, key, seconds <= 0 ? 0 : now + seconds * 1000, 0);
}

bool json_key::encode_expiry(shared_db& db, std::string& bytes,
    long long deadline, bool paged, bool& preserve_pages) {
    if (!paged) {
        value_record record;
        if (!materialize(record)) return false;
        record.header.expires_at_ms = deadline;
        return encode(db, record, bytes);
    }
    if (bytes.compare(0, 3, "PSR") == 0) {
        string_root root;
        if (string_page_codec::decode_root(bytes, root) !=
            string_page_status::ok) {
            read_status_ = read_status::invalid_record; return false;
        }
        root.expires_at_ms = deadline;
        if (string_page_codec::encode_root(root, bytes) !=
            string_page_status::ok) {
            read_status_ = read_status::storage_error; return false;
        }
        preserve_pages = true;
        return true;
    }
    if (bytes.compare(0, 3, "PXR") == 0) {
        stream_root root;
        if (!decode_stream_root(bytes, root)) {
            read_status_ = read_status::invalid_record; return false;
        }
        root.expires_at_ms = deadline;
        encode_stream_root(root, bytes);
        preserve_pages = true;
        return true;
    }
    if (bytes.compare(0, 3, "PZR") == 0) {
        if (compact_zset::matches(bytes)) {
            try {
                const compact_zset::view packed(bytes);
                packed.replace_deadline(bytes, deadline);
                preserve_pages = true;
                return true;
            } catch (const std::runtime_error&) {
                read_status_ = read_status::invalid_record; return false;
            }
        }
        zset_root root;
        if (zset_page_codec::decode_root(bytes, root) != map_page_status::ok) {
            read_status_ = read_status::invalid_record; return false;
        }
        root.expires_at_ms = deadline;
        if (zset_page_codec::encode_root(root, bytes) != map_page_status::ok) {
            read_status_ = read_status::storage_error; return false;
        }
        preserve_pages = true;
        return true;
    }
    if (bytes.compare(0, 3, "PMR") == 0) {
        if (compact_set::matches(bytes)) {
            try {
                const compact_set::view packed(bytes);
                packed.replace_deadline(bytes, deadline);
                preserve_pages = true;
                return true;
            } catch (const std::runtime_error&) {
                read_status_ = read_status::invalid_record; return false;
            }
        }
        if (compact_hash::matches(bytes)) {
            try {
                const compact_hash::view packed(bytes);
                packed.replace_deadline(bytes, deadline);
                preserve_pages = true;
                return true;
            } catch (const std::runtime_error&) {
                read_status_ = read_status::invalid_record; return false;
            }
        }
        if (packed_hash::matches(bytes)) {
            packed_hash::root root;
            if (packed_hash::decode_root(bytes, root) != map_page_status::ok) {
                read_status_ = read_status::invalid_record; return false;
            }
            root.expires_at_ms = deadline;
            if (packed_hash::encode_root(root, bytes) != map_page_status::ok) {
                read_status_ = read_status::storage_error; return false;
            }
            preserve_pages = true; return true;
        }
        map_root root;
        if (map_page_codec::decode_root(bytes, root) != map_page_status::ok) {
            read_status_ = read_status::invalid_record; return false;
        }
        root.expires_at_ms = deadline;
        if (map_page_codec::encode_root(root, bytes) != map_page_status::ok) {
            read_status_ = read_status::storage_error; return false;
        }
        preserve_pages = true;
        return true;
    }
    if (compact_list_root::matches(bytes)) {
        try {
            const compact_list_root::view packed(bytes);
            packed.replace_deadline(bytes, deadline);
            preserve_pages = true;
            return true;
        } catch (const std::runtime_error&) {
            read_status_ = read_status::invalid_record; return false;
        }
    }
    list_root root;
    if (list_page_codec::decode_root(bytes, root) != list_page_status::ok) {
        read_status_ = read_status::invalid_record;
        return false;
    }
    root.expires_at_ms = deadline;
    const auto result = list_page_codec::encode_root(root, bytes);
    if (result != list_page_status::ok) {
        read_status_ = read_status::storage_error;
        return false;
    }
    preserve_pages = true;
    return true;
}

bool json_key::expiry_time(shared_db& db, const std::string& key, long long& ms) {
    if (!read(db, key)) return false;
    ms = view_->header().expires_at_ms;
    return true;
}

bool json_key::pttl(shared_db& db, const std::string& key, long long& ms) {
    if (!read(db, key)) return false;
    const auto deadline = view_->header().expires_at_ms;
    ms = deadline < 0 ? -1 : deadline - unix_time_ms();
    if (deadline >= 0 && ms < 0) ms = 0;
    return true;
}

bool json_key::ttl(shared_db& db, const std::string& key, long long& seconds) {
    long long ms;
    if (!pttl(db, key, ms)) return false;
    seconds = ms < 0 ? ms : ms / 1000 + (ms % 1000 >= 500);
    return true;
}

bool json_key::scan(shared_db& db, db_cursor& cursor,
        std::vector<std::string>& keys, size_t max) {
    return db->scan(cursor, keys, max);
}

} // namespace dao
} // namespace pkv
