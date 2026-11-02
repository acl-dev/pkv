#pragma once
#include <string>

namespace pkv {
// Binary-safe Redis-style glob matching. Constant stack space, no recursion.
bool redis_glob_match(const std::string& pattern, const std::string& value);
}
