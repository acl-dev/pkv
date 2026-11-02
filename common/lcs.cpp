#include "lcs.h"
#include "redis_integer.h"
#include <algorithm>
#include <limits>
#include <new>
#include <strings.h>
#include <cstring>

namespace pkv {
namespace {
bool named(const std::string& text, const char* name) {
    return text.size() == std::strlen(name) &&
        !strncasecmp(text.data(), name, text.size());
}
const char* allocation_error =
    "ERR Insufficient memory, failed allocating transient memory for LCS";
void trace(const std::string& first, size_t columns,
    const std::vector<unsigned char>& directions, size_t i, size_t j,
    const lcs_options& options, lcs_result& result) {
    size_t output = result.length;
    bool active = false;
    lcs_match match = {};
    const auto emit = [&] {
        if (active && match.first_end - match.first_begin + 1 >=
            options.minimum) result.matches.push_back(match);
        active = false;
    };
    if (!options.indexes) result.value.resize(output);
    while (i && j) {
        const size_t cell = (i - 1) * columns + j - 1;
        const auto direction = (directions[cell / 4] >> (cell % 4 * 2)) & 3;
        if (direction == 0) {
            --i; --j;
            if (!options.indexes) result.value[--output] = first[i];
            else {
                if (!active) {
                    match = {static_cast<uint32_t>(i),
                        static_cast<uint32_t>(i), static_cast<uint32_t>(j),
                        static_cast<uint32_t>(j)};
                    active = true;
                } else {
                    match.first_begin = static_cast<uint32_t>(i);
                    match.second_begin = static_cast<uint32_t>(j);
                }
            }
        } else {
            if (options.indexes) emit();
            if (direction == 1) --i;
            else --j;
        }
    }
    if (options.indexes) emit();
}
} // namespace
const char* parse_lcs_options(const std::vector<const std::string*>& arguments,
    lcs_options& options) {
    options = lcs_options();
    for (size_t i = 0; i < arguments.size(); ++i) {
        const auto& text = *arguments[i];
        if (named(text, "LEN")) options.length_only = true;
        else if (named(text, "IDX")) options.indexes = true;
        else if (named(text, "WITHMATCHLEN")) options.with_length = true;
        else if (named(text, "MINMATCHLEN") && i + 1 < arguments.size()) {
            long long value;
            if (!parse_redis_integer(*arguments[++i], value))
                return "ERR value is not an integer or out of range";
            options.minimum = value < 0 ? 0 : static_cast<uint64_t>(value);
        } else return "ERR syntax error";
    }
    return options.length_only && options.indexes ?
        "ERR If you want both the length and indexes, please just use IDX." :
        nullptr;
}
const char* lcs_dimensions(uint64_t first, uint64_t second) {
    if (first >= UINT32_MAX - 1ULL || second >= UINT32_MAX - 1ULL)
        return "ERR String too long for LCS";
    // Keep Redis's default transient-table admission bound even though our
    // one row and two-bit traceback use substantially less memory.
    if ((first + 1) * (second + 1) > (512ULL * 1024 * 1024 / 4))
        return "ERR Insufficient memory, transient memory for LCS exceeds "
            "proto-max-bulk-len";
    return nullptr;
}
const char* longest_common_subsequence(const std::string& first,
    const std::string& second, const lcs_options& options, lcs_result& result) {
    result = lcs_result();
    if (const auto* error = lcs_dimensions(first.size(), second.size()))
        return error;
    if (first.empty() || second.empty()) return nullptr;
    try {
        // LEN needs only one row; choose the shorter row dimension.
        const auto& a = options.length_only && first.size() < second.size() ?
            second : first;
        const auto& b = options.length_only && first.size() < second.size() ?
            first : second;
        std::vector<uint32_t> row(b.size() + 1);
        std::vector<unsigned char> directions;
        if (!options.length_only)
            directions.resize((a.size() * b.size() + 3) / 4);
        for (size_t i = 1; i <= a.size(); ++i) {
            uint32_t diagonal = 0;
            for (size_t j = 1; j <= b.size(); ++j) {
                const auto above = row[j];
                unsigned char direction = 0;
                if (a[i - 1] == b[j - 1]) row[j] = diagonal + 1;
                else if (above > row[j - 1]) direction = 1;
                else { row[j] = row[j - 1]; direction = 2; }
                diagonal = above;
                if (!options.length_only) {
                    const size_t cell = (i - 1) * b.size() + j - 1;
                    directions[cell / 4] |= direction << (cell % 4 * 2);
                }
            }
        }
        result.length = row.back();
        if (!options.length_only)
            trace(first, b.size(), directions, a.size(), b.size(), options,
                result);
    } catch (const std::bad_alloc&) {
        result = lcs_result(); return allocation_error;
    }
    return nullptr;
}
} // namespace pkv
