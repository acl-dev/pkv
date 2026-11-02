#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace pkv {
struct lcs_options {
    bool length_only = false;
    bool indexes = false;
    bool with_length = false;
    uint64_t minimum = 0;
};
struct lcs_match {
    uint32_t first_begin, first_end, second_begin, second_end;
};
struct lcs_result {
    uint32_t length = 0;
    std::string value;
    std::vector<lcs_match> matches;
};
const char* parse_lcs_options(const std::vector<const std::string*>& arguments,
    lcs_options& options);
const char* lcs_dimensions(uint64_t first, uint64_t second);
const char* longest_common_subsequence(const std::string& first,
    const std::string& second, const lcs_options& options, lcs_result& result);
} // namespace pkv
