#include "zset_range.h"
#include "redis_double.h"

namespace pkv {
bool parse_zscore_bound(const std::string& text, zscore_bound& out) {
    out = zscore_bound();
    out.exclusive = !text.empty() && text.front() == '(';
    return parse_redis_score_endpoint(out.exclusive ? text.substr(1) : text, out.value);
}
bool parse_zlex_bound(const std::string& text, zlex_bound& out) {
    out = zlex_bound();
    if (text == "+") { out.infinity = 1; return true; }
    if (text == "-") { out.infinity = -1; return true; }
    if (text.empty() || (text.front() != '[' && text.front() != '(')) return false;
    out.exclusive = text.front() == '(';
    out.value = text.substr(1);
    return true;
}
}
