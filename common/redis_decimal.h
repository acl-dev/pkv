#pragma once
#include <cstddef>
#include <string>

namespace pkv {

// Redis string2ld / LD_STR_HUMAN limits and formatting, separate from ZSet's
// shortest-double score representation. Nonzero subnormals are accepted.
constexpr size_t redis_decimal_limit = 5 * 1024;
bool parse_redis_decimal(const std::string& text, long double& out);
bool format_redis_decimal(long double value, std::string& out);

} // namespace pkv
