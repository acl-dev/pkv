#include "redis_glob.h"
#include <algorithm>

namespace pkv {
namespace {
bool atom(const std::string& pattern, size_t begin, unsigned char value, size_t& end) {
    end = begin + 1;
    const auto ch = static_cast<unsigned char>(pattern[begin]);
    if (ch == '?') return true;
    if (ch == '\\' && end < pattern.size()) return static_cast<unsigned char>(pattern[end++]) == value;
    if (ch != '[') return ch == value;
    size_t i = end;
    const bool negate = i < pattern.size() && pattern[i] == '^';
    if (negate) ++i;
    bool hit = false;
    while (i < pattern.size() && pattern[i] != ']') {
        if (pattern[i] == '\\' && i + 1 < pattern.size()) {
            hit = hit || static_cast<unsigned char>(pattern[i + 1]) == value;
            i += 2;
        } else if (i + 2 < pattern.size() && pattern[i + 1] == '-') {
            // Redis's byte ranges use signed char order on its 64-bit Unix
            // builds. Make that behavior independent of this compiler's char.
            const int a = static_cast<signed char>(pattern[i]);
            const int b = static_cast<signed char>(pattern[i + 2]);
            const int c = static_cast<signed char>(value);
            hit = hit || (c >= std::min(a, b) && c <= std::max(a, b));
            i += 3;
        } else {
            hit = hit || static_cast<unsigned char>(pattern[i]) == value;
            ++i;
        }
    }
    end = i < pattern.size() ? i + 1 : i;
    return negate ? !hit : hit;
}
}

bool redis_glob_match(const std::string& pattern, const std::string& value) {
    size_t p = 0, s = 0, retry = 0, star = std::string::npos;
    while (s < value.size()) {
        if (p < pattern.size() && pattern[p] == '*') {
            while (p < pattern.size() && pattern[p] == '*') ++p;
            if (p == pattern.size()) return true;
            star = p;
            retry = s;
            continue;
        }
        size_t next = p;
        if (p < pattern.size() && atom(pattern, p, static_cast<unsigned char>(value[s]), next)) {
            p = next;
            ++s;
        } else if (star != std::string::npos && retry < value.size()) {
            p = star;
            s = ++retry;
        } else return false;
    }
    while (p < pattern.size() && pattern[p] == '*') ++p;
    return p == pattern.size();
}

} // namespace pkv
