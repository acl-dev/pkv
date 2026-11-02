#include "redis_decimal.h"
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <locale.h>
#ifdef __APPLE__
#include <xlocale.h>
#endif

namespace pkv {
namespace {
struct decimal_locale {
#ifdef _WIN32
    _locale_t value = _create_locale(LC_NUMERIC, "C");
    ~decimal_locale() { if (value) _free_locale(value); }
#else
    locale_t value = newlocale(LC_NUMERIC_MASK, "C", nullptr);
    ~decimal_locale() { if (value) freelocale(value); }
#endif
};
const decimal_locale& locale() {
    static const decimal_locale numeric;
    return numeric;
}
#ifndef _WIN32
struct scoped_locale {
    locale_t previous;
    explicit scoped_locale(locale_t value) : previous(uselocale(value)) {}
    ~scoped_locale() { if (previous) uselocale(previous); }
};
#endif
}

bool parse_redis_decimal(const std::string& text, long double& out) {
    if (text.empty() || text.size() >= redis_decimal_limit ||
        std::isspace(static_cast<unsigned char>(text.front()))) return false;
    const auto& numeric = locale();
    if (!numeric.value) return false;
    errno = 0;
    char* end = nullptr;
#ifdef _WIN32
    const auto value = _strtold_l(text.c_str(), &end, numeric.value);
#else
    const auto value = strtold_l(text.c_str(), &end, numeric.value);
#endif
    if (end == text.c_str() || end != text.data() + text.size() ||
        std::isnan(value) || errno == EINVAL || (errno == ERANGE &&
        (std::isinf(value) || value == 0))) return false;
    out = value;
    return true;
}

bool format_redis_decimal(long double value, std::string& out) {
    out.clear();
    if (!std::isfinite(value)) return false;
    const auto& numeric = locale();
    if (!numeric.value) return false;
    char buffer[redis_decimal_limit];
#ifdef _WIN32
    const int size = _snprintf_l(buffer, sizeof(buffer), "%.17Lf",
        numeric.value, value);
#else
    scoped_locale guard(numeric.value);
    if (!guard.previous) return false;
    const int size = std::snprintf(buffer, sizeof(buffer), "%.17Lf", value);
#endif
    if (size <= 0 || static_cast<size_t>(size) >= sizeof(buffer)) return false;
    size_t length = static_cast<size_t>(size);
    while (length && buffer[length - 1] == '0') --length;
    if (length && buffer[length - 1] == '.') --length;
    if (length == 2 && buffer[0] == '-' && buffer[1] == '0') {
        buffer[0] = '0'; length = 1;
    }
    out.assign(buffer, length);
    return true;
}

} // namespace pkv
