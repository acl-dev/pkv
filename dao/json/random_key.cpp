#include "stdafx.h"
#include "json_scan.h"

namespace pkv {
namespace dao {
bool json_scan::random_key(shared_db& db, std::string& out, bool& found) {
    out.clear(); found = false;
    execution_guard snapshot(true);
    try {
        uint64_t previous = UINT64_MAX;
        unsigned replica_tries = 0;
        for (;;) {
            const auto status = db->sample_key(out, found);
            if (status != db::scan_status::ok) {
                read_status_ = status == db::scan_status::unsupported ?
                    read_status::unsupported_operation :
                    read_status::storage_error;
                return false;
            }
            if (!found) { reset(); return true; }
            record_header header;
            if (read_header(db, out, header)) return true;
            if (read_status_ == read_status::missing &&
                execution_replica_read()) {
                // Expired roots remain physical on replicas. Rejection must
                // not depend on a shrinking key count or retry indefinitely.
                if (++replica_tries < 100) {
                    out.clear(); found = false; continue;
                }
                std::string candidate;
                candidate.swap(out); found = false;
                const auto live = db->sample_live_key(out, found);
                if (live == db::scan_status::ok) {
                    // Redis permits a retained expired name when no live
                    // roots remain. Nil is reserved for an empty database.
                    if (!found) { out.swap(candidate); found = true; }
                    reset();
                    if (found) read_status_ = read_status::ok;
                    return true;
                }
                read_status_ = live == db::scan_status::unsupported ?
                    read_status::unsupported_operation :
                    read_status::storage_error;
                return false;
            }
            out.clear(); found = false;
            if (read_status_ != read_status::missing) return false;
            uint64_t remaining = 0;
            if (db->key_count(remaining) != db::count_status::ok ||
                remaining >= previous) {
                read_status_ = read_status::storage_error; return false;
            }
            previous = remaining;
        }
    } catch (const std::bad_alloc&) {
        out.clear(); found = false;
        read_status_ = read_status::storage_error; return false;
    }
}
} // namespace dao
} // namespace pkv
