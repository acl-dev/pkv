#include "stdafx.h"
#include "redis_string.h"
#include "redis_handler.h"
#include "common/redis_integer.h"
#include <climits>
#include <cstring>

namespace pkv {
namespace {
bool option_is(const std::string& text, const char* name) {
    return text.size() == std::strlen(name) &&
        !strncasecmp(text.data(), name, text.size());
}
const char* parse_msetex(const redis_object& request, size_t& pairs,
    dao::set_options& options) {
    long long count;
    if (!parse_redis_integer(*request.argument(1), count) || count < 1 ||
        count > INT_MAX) return "ERR invalid numkeys value";
    if (static_cast<unsigned long long>(count) > (request.size() - 2) / 2)
        return "ERR wrong number of key-value pairs";
    pairs = static_cast<size_t>(count);
    enum expiry_mode { none, ex, px, exat, pxat, keep } mode = none;
    const std::string* expiry = nullptr;
    for (size_t i = 2 + pairs * 2; i < request.size(); ++i) {
        const auto& option = *request.argument(i);
        if (option_is(option, "NX") || option_is(option, "XX")) {
            const auto next = option_is(option, "NX") ?
                dao::set_options::only_missing :
                dao::set_options::only_existing;
            if (options.when != dao::set_options::any && options.when != next)
                return "ERR syntax error";
            options.when = next;
        } else if (option_is(option, "KEEPTTL")) {
            if (mode != none && mode != keep) return "ERR syntax error";
            mode = keep; options.keep_ttl = true;
        } else {
            expiry_mode next = none;
            if (option_is(option, "EX")) next = ex;
            else if (option_is(option, "PX")) next = px;
            else if (option_is(option, "EXAT")) next = exat;
            else if (option_is(option, "PXAT")) next = pxat;
            if (next == none || (mode != none && mode != next) ||
                i + 1 == request.size()) return "ERR syntax error";
            mode = next; expiry = request.argument(++i);
        }
    }
    if (expiry) {
        long long amount;
        if (!parse_redis_integer(*expiry, amount))
            return "ERR value is not an integer or out of range";
        if (amount <= 0 || !expiration_deadline(amount,
            mode == ex || mode == exat ? 1000 : 1,
            mode == exat || mode == pxat ? 0 : unix_time_ms(),
            options.expires_at_ms))
            return "ERR invalid expire time in 'msetex' command";
    }
    return nullptr;
}
} // namespace
bool redis_string::msetex(redis_coder& result) {
    size_t pairs = 0;
    dao::set_options options;
    if (const auto* error = parse_msetex(obj_, pairs, options)) {
        result.create_object().set_error(error); return true;
    }
    dao::string_assignments values;
    for (size_t i = 2; i < 2 + pairs * 2; i += 2)
        values[*obj_.argument(i)] = obj_.argument(i + 1);
    bool applied;
    if (!dao_->msetex(handler_.get_db(), values, options, applied))
        result.create_object().set_error(dao_->error());
    else result.create_object().set_number(applied ? 1 : 0);
    return true;
}
} // namespace pkv
