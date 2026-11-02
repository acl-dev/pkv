#include "stdafx.h"
#include "json_scan.h"

namespace pkv {
namespace dao {
bool json_scan::read_header(shared_db& db, const std::string& key,
    record_header& out) {
    reset();
    bool valid = false;
    const bool ok = db->update(key, [&](bool found, std::string& bytes) {
        if (!found) return db::update_action::keep;
        const auto status = db->value_codec().stored_header(bytes, out);
        if (status != codec_status::ok) {
            read_status_ = status == codec_status::unsupported_version ?
                read_status::unsupported_format :
                status == codec_status::invalid_record ?
                    read_status::invalid_record : read_status::storage_error;
            return db::update_action::keep;
        }
        if (out.expires_at_ms >= 0 && out.expires_at_ms <= unix_time_ms())
            return db::update_action::erase;
        read_status_ = read_status::ok;
        valid = true;
        return db::update_action::keep;
    });
    if (!ok) read_status_ = read_status::storage_error;
    return ok && valid;
}
bool json_scan::all_keys(shared_db& db, const std::string& pattern,
    std::unordered_set<std::string>& out) {
    out.clear();
    // A full keyspace command must see one write/expiry boundary, unlike
    // independent SCAN pages. Handler admission selects this mode up front.
    execution_guard snapshot(true);
    std::unordered_set<std::string> pending;
    scan_options options;
    options.count = 256;
    options.has_match = true;
    options.pattern = pattern;
    do {
        scan_page page;
        if (!keys(db, options, page)) return false;
        // A workspace may emit an overwritten source key on two pages.
        for (auto& key : page.values) pending.emplace(std::move(key));
        if (page.cursor && page.cursor == options.cursor) {
            read_status_ = read_status::storage_error; return false;
        }
        options.cursor = page.cursor;
    } while (options.cursor);
    out.swap(pending);
    return true;
}
} // namespace dao
} // namespace pkv
