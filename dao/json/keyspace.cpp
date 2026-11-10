#include "stdafx.h"
#include "json_scan.h"

namespace pkv {
namespace dao {
bool json_scan::stored_header(const record_codec& codec,
    const std::string& bytes, record_header& out) {
    const auto status = codec.stored_header(bytes, out);
    if (status == codec_status::ok) return true;
    read_status_ = status == codec_status::unsupported_version ?
        read_status::unsupported_format : status == codec_status::invalid_record ?
            read_status::invalid_record : read_status::storage_error;
    return false;
}
bool json_scan::read_header(shared_db& db, const std::string& key,
    record_header& out) {
    reset();
    bool valid = false;
    const bool ok = db->update(key, [&](bool found, std::string& bytes) {
        if (!found) return db::update_action::keep;
        if (!stored_header(db->value_codec(), bytes, out))
            return db::update_action::keep;
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
bool json_scan::visit_all_keys(shared_db& db, const std::string& pattern,
    const std::function<void(const std::string&)>& emit) {
    reset();
    execution_guard snapshot(true);
    const auto now = unix_time_ms();
    const bool all = pattern == "*";
    const auto& codec = db->value_codec();
    std::vector<std::string> expired;
    const auto status = db->visit_root_records([&](const std::string& key,
        const std::string& bytes) {
        if (!all && !redis_glob_match(pattern, key)) return true;
        record_header header;
        if (!stored_header(codec, bytes, header)) return false;
        if (header.expires_at_ms >= 0 && header.expires_at_ms <= now)
            expired.push_back(key);
        else
            emit(key);
        return true;
    });
    if (status == db::scan_status::unsupported) {
        std::unordered_set<std::string> keys;
        if (!all_keys(db, pattern, keys)) return false;
        for (const auto& key : keys) emit(key);
        return true;
    }
    if (status != db::scan_status::ok) read_status_ = read_status::storage_error;
    if (read_status_ != read_status::missing) return false;
    // Delete expired roots after the visitor releases the borrowed records.
    for (const auto& key : expired) {
        record_header header;
        if (read_header(db, key, header)) emit(key);
        else if (read_status_ != read_status::missing) return false;
    }
    read_status_ = read_status::ok;
    return true;
}
} // namespace dao
} // namespace pkv
