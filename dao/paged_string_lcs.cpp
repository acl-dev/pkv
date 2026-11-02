#include "stdafx.h"
#include "paged_string.h"
#include "string_bitop_source.h"

namespace pkv {
namespace dao {
bool paged_string::lcs(shared_db& db, const std::string& first,
    const std::string& second, const std::vector<const std::string*>& arguments,
    lcs_options& options, lcs_result& out, const char*& error) {
    reset(); out = lcs_result(); error = nullptr;
    const auto status = db->transact_pages({first, second},
        [&](page_transaction& tx) {
        const auto now = unix_time_ms();
        string_bitop_source a(db, tx, first, now), b(db, tx, second, now);
        if (!a.open() || !b.open()) {
            read_status_ = a.error() != read_status::ok ? a.error() : b.error();
            if (read_status_ == read_status::wrong_type)
                error = "ERR The specified keys must contain string values";
            return false;
        }
        // Redis resolves types before option errors. Validate dimensions
        // before copying any payload into the algorithm's input buffers.
        error = parse_lcs_options(arguments, options);
        if (!error) error = lcs_dimensions(a.length(), b.length());
        if (error) return false;
        std::string left, right;
        if (!a.read(0, static_cast<size_t>(a.length()), left) ||
            !b.read(0, static_cast<size_t>(b.length()), right)) {
            read_status_ = a.error() != read_status::ok ? a.error() : b.error();
            return false;
        }
        error = longest_common_subsequence(left, right, options, out);
        return !error;
    });
    if (result(status)) return true;
    out = lcs_result(); return false;
}
} // namespace dao
} // namespace pkv
