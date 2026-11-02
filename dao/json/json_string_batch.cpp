#include "stdafx.h"
#include "json_string.h"

namespace pkv {
namespace dao {
bool json_string::msetex(shared_db& db, const string_assignments& values,
    const set_options& options, bool& applied) {
    reset(); applied = false;
    if (values.empty() || (options.when != set_options::any &&
        options.when != set_options::only_missing &&
        options.when != set_options::only_existing)) {
        read_status_ = read_status::unsupported_operation; return false;
    }
    std::vector<std::string> keys;
    for (const auto& value : values) {
        if (!value.second || value.second->size() > redis_string_max) {
            read_status_ = read_status::string_too_large; return false;
        }
        keys.push_back(value.first);
    }
    const auto status = db->transact(keys, [&](db::batch_view entries) {
        const auto now = unix_time_ms();
        if (options.when != set_options::any) {
            for (const auto& entry : entries) {
                bool live = entry.found;
                if (live && !decode(db, entry.value, now)) {
                    if (read_status_ != read_status::missing) return false;
                    live = false;
                }
                if ((options.when == set_options::only_missing && live) ||
                    (options.when == set_options::only_existing && !live))
                    return true;
            }
        }
        for (auto& entry : entries) {
            value_record record;
            record.header.expires_at_ms = options.expires_at_ms;
            if (options.keep_ttl) {
                record.header.expires_at_ms = -1;
                if (entry.found) {
                    if (decode(db, entry.value, now))
                        record.header.expires_at_ms =
                            view_->header().expires_at_ms;
                    else if (read_status_ != read_status::missing)
                        return false;
                }
            }
            record.string_value = *values.at(entry.key);
            if (!encode(db, record, entry.value)) return false;
            entry.action = db::update_action::write;
        }
        applied = true; return true;
    });
    if (status == db::batch_status::ok) return true;
    read_status_ = status == db::batch_status::unsupported ?
        read_status::unsupported_operation : read_status::storage_error;
    applied = false; return false;
}
} // namespace dao
} // namespace pkv
