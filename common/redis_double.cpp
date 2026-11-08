#include "redis_double.h"
#include <cerrno>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <locale.h>
#ifdef __APPLE__
#include <xlocale.h>
#endif
extern "C" {
#include "third_party/fpconv/fpconv_dtoa.h"
}

namespace pkv {
namespace {
struct numeric_locale {
#ifdef _WIN32
    _locale_t value = _create_locale(LC_NUMERIC, "C");
    ~numeric_locale() { if (value) _free_locale(value); }
#else
    locale_t value = newlocale(LC_NUMERIC_MASK, "C", nullptr);
    ~numeric_locale() { if (value) freelocale(value); }
#endif
};
}
static bool parse_double(const std::string& text, double& out, bool endpoint) {
    // The reference endpoint parser treats an empty bound as zero, unlike ZADD.
    if (text.empty()) { if (endpoint) { out = 0; return true; } return false; }
    size_t first = text.front() == '+' || text.front() == '-' ? 1 : 0;
    // All integers of at most 15 decimal digits are exact in binary64.
    if (text.size() > first && text.size() - first <= 15) {
        uint64_t integer = 0;
        size_t pos = first;
        for (; pos < text.size(); ++pos) {
            const auto digit = static_cast<unsigned>(text[pos] - '0');
            if (digit > 9) break;
            integer = integer * 10 + digit;
        }
        if (pos == text.size()) {
            const auto value = static_cast<double>(integer);
            out = text.front() == '-' ? -value : value;
            return true;
        }
    }
    if (endpoint && text.size() > first + 1 && text[first] == '0' && (text[first + 1] == 'x' || text[first + 1] == 'X')) return false;
    if (std::isspace(static_cast<unsigned char>(text.front()))) return false;
    static const numeric_locale locale;
    if (!locale.value) return false;
    char* end = nullptr;
    errno = 0;
#ifdef _WIN32
    const auto value = _strtod_l(text.c_str(), &end, locale.value);
#else
    const auto value = strtod_l(text.c_str(), &end, locale.value);
#endif
    if (end == text.c_str() || end != text.c_str() + text.size() || std::isnan(value) || errno == EINVAL ||
        (!endpoint && errno == ERANGE && (std::isinf(value) || value == 0))) return false;
    out = value;
    return true;
}
bool parse_redis_double(const std::string& text, double& out) { return parse_double(text, out, false); }
bool parse_redis_score_endpoint(const std::string& text, double& out) { return parse_double(text, out, true); }
std::string format_redis_double(double value) {
    if (std::isnan(value)) return "nan";
    if (std::isinf(value)) return std::signbit(value) ? "-inf" : "inf";
    if (value == 0) return std::signbit(value) ? "-0" : "0";
    const auto integer_limit = std::ldexp(1.0, 62);
    if (value >= -integer_limit && value <= integer_limit && std::trunc(value) == value)
        return std::to_string(static_cast<long long>(value));
    char buffer[32];
    const int length = fpconv_dtoa(value, buffer);
    return std::string(buffer, static_cast<size_t>(length));
}
}
