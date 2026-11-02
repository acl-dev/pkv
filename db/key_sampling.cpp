#include "stdafx.h"
#include "db.h"
#include "common/set_sampling.h"
#include "coder/record_codec.h"

namespace pkv {
db::scan_status db::dbkey_at_rank(uint64_t rank, std::string& out,
    bool& found) {
    return dbvisit_keys([&](const std::string& key) {
        if (rank) { --rank; return true; }
        out = key; found = true; return false;
    });
}
db::scan_status db::key_at_rank(uint64_t rank, std::string& out,
    bool& found) {
    out.clear(); found = false;
    execution_guard snapshot(true);
    try {
        const auto status = dbkey_at_rank(rank, out, found);
        if (status == scan_status::ok) return status;
        out.clear(); found = false; return status;
    } catch (const std::bad_alloc&) {
        out.clear(); found = false; return scan_status::error;
    }
}
db::scan_status db::sample_key(std::string& out, bool& found) {
    out.clear(); found = false;
    execution_guard snapshot(true);
    try {
        uint64_t count = 0;
        const auto status = dbkey_count(count);
        if (status == count_status::unsupported)
            return scan_status::unsupported;
        if (status != count_status::ok) return scan_status::error;
        if (!count) return scan_status::ok;
        const auto rank = std::uniform_int_distribution<uint64_t>(0,
            count - 1)(set_sampling_engine());
        const auto selected = dbkey_at_rank(rank, out, found);
        if (selected == scan_status::ok && found) return selected;
        out.clear(); found = false;
        return selected == scan_status::ok ? scan_status::error : selected;
    } catch (const std::bad_alloc&) {
        out.clear(); found = false; return scan_status::error;
    }
}
db::scan_status db::sample_live_key(std::string& out, bool& found) {
    out.clear(); found = false;
    execution_guard snapshot(true);
    try {
        uint64_t seen = 0;
        bool valid = true;
        const auto now = unix_time_ms();
        const auto status = dbvisit_keys([&](const std::string& key) {
            std::string bytes;
            record_header header;
            if (dblookup(key, bytes) != lookup_status::found ||
                codec_->stored_header(bytes, header) != codec_status::ok) {
                valid = false; return false;
            }
            if (header.expires_at_ms >= 0 && header.expires_at_ms <= now)
                return true;
            if (seen == UINT64_MAX) { valid = false; return false; }
            ++seen;
            if (std::uniform_int_distribution<uint64_t>(1, seen)(
                set_sampling_engine()) == 1) { out = key; found = true; }
            return true;
        });
        if (valid && status == scan_status::ok) return status;
        out.clear(); found = false;
        return status == scan_status::ok ? scan_status::error : status;
    } catch (const std::bad_alloc&) {
        out.clear(); found = false; return scan_status::error;
    }
}
} // namespace pkv
