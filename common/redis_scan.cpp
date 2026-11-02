#include "redis_scan.h"
#include "redis_integer.h"
#include <cerrno>
#include <cstdlib>

namespace pkv {
bool parse_scan_cursor(const std::string& text, unsigned long long& out) {
    long long signed_value;
    if (parse_redis_integer(text, signed_value)) {
        if (signed_value < 0) return false;
        out = static_cast<unsigned long long>(signed_value);
        return true;
    }
    if (text.empty()) return false;
    char* end = nullptr;
    errno = 0;
    const auto value = std::strtoull(text.c_str(), &end, 10);
    if (errno == ERANGE || end == text.c_str() || end != text.c_str() + text.size()) return false;
    out = value;
    return true;
}
}
