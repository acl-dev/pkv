#include "stdafx.h"
#include "paged_string.h"

namespace pkv {
namespace dao {
namespace {
bool live_header(shared_db& db, const db::batch_entry& entry, int64_t now,
    int64_t& expiry, bool& live, dao_base::read_status& error) {
    live = false; expiry = -1;
    if (!entry.found) return true;
    record_header header;
    const auto status = db->value_codec().stored_header(entry.value, header);
    if (status != codec_status::ok) {
        error = status == codec_status::unsupported_version ?
            dao_base::read_status::unsupported_format :
            status == codec_status::invalid_record ?
                dao_base::read_status::invalid_record :
                dao_base::read_status::storage_error;
        return false;
    }
    live = header.expires_at_ms < 0 || header.expires_at_ms > now;
    if (live) expiry = header.expires_at_ms;
    return true;
}
} // namespace
bool paged_string::msetex(shared_db& db, const string_assignments& values,
    const set_options& options, bool& applied) {
    reset(); applied = false;
    if (values.empty() || (options.when != set_options::any &&
        options.when != set_options::only_missing &&
        options.when != set_options::only_existing)) {
        read_status_ = read_status::unsupported_operation; return false;
    }
    std::vector<std::string> keys;
    keys.reserve(values.size());
    for (const auto& value : values) {
        if (!value.second || value.second->size() > redis_string_max) {
            read_status_ = read_status::string_too_large; return false;
        }
        keys.push_back(value.first);
    }
    const auto status = db->transact_pages(keys, [&](page_transaction& tx) {
        const auto now = unix_time_ms();
        if (options.when != set_options::any) {
            for (const auto& entry : tx.roots()) {
                bool live;
                int64_t expiry;
                if (!live_header(db, entry, now, expiry, live, read_status_))
                    return false;
                if ((options.when == set_options::only_missing && live) ||
                    (options.when == set_options::only_existing && !live))
                    return true;
            }
        }
        for (const auto& value : values) {
            int64_t expiry = options.expires_at_ms;
            if (options.keep_ttl) {
                bool live;
                if (!live_header(db, *tx.roots().find(value.first), now,
                    expiry, live, read_status_)) return false;
            }
            string_store store(tx, value.first);
            if (!store.assign(*value.second) || !store.set_expiry(expiry) ||
                !store.finish()) return stored(store);
        }
        applied = true; return true;
    });
    if (result(status)) return true;
    applied = false; return false;
}
} // namespace dao
} // namespace pkv
