#include "command_keys.h"
#include "geo_radius.h"
#include "command_catalog.h"
#include "redis_integer.h"
#include <cstring>
#include <algorithm>
#include <climits>
#include <strings.h>

namespace pkv {
namespace {
bool named(const char* command, const char* names) {
    const size_t size = std::strlen(command);
    for (const char* start = names; *start;) {
        const char* end = std::strchr(start, ' ');
        const size_t length = end ? static_cast<size_t>(end - start) :
            std::strlen(start);
        if (size == length && !strncasecmp(command, start, size)) return true;
        if (!end) break;
        start = end + 1;
    }
    return false;
}
} // namespace

bool command_keys(std::vector<const std::string*>& arguments) {
    if (arguments.empty()) return false;
    const auto* command = arguments.front()->c_str();
    const auto radius = radius_mode(command);
    if (radius != geo_radius_mode::none || named(command,
        "BLPOP BRPOP BLMPOP BLMOVE BRPOPLPUSH BZPOPMIN BZPOPMAX BZMPOP")) {
        const auto* entry = command_catalog(*arguments.front());
        std::vector<command_key_argument> found;
        if (!entry || !command_key_arguments(*entry, arguments, found, true)) {
            arguments.clear(); return false;
        }
        arguments.clear();
        for (const auto& key : found) arguments.push_back(key.key);
        return true;
    }
    size_t first = 1, end = arguments.size(), stride = 1;
    const bool destination = named(command,
        "ZUNIONSTORE ZINTERSTORE ZDIFFSTORE");
    if (named(command, "PING ECHO QUIT SELECT CONFIG COMMAND CLUSTER SCAN KEYS "
        "DBSIZE RANDOMKEY TIME CLIENT RESET HELLO AUTH ACL INFO "
        "MULTI EXEC DISCARD UNWATCH SUBSCRIBE UNSUBSCRIBE PSUBSCRIBE "
        "PUNSUBSCRIBE SSUBSCRIBE SUNSUBSCRIBE PUBLISH SPUBLISH PUBSUB")) {
        arguments.clear(); return true;
    }
    if (named(command, "MSETEX")) {
        long long count;
        if (arguments.size() < 2 ||
            !parse_redis_integer(*arguments[1], count) || count < 1 ||
            count > INT_MAX || static_cast<unsigned long long>(count) >
                (arguments.size() - 2) / 2) {
            arguments.clear(); return false;
        }
        first = 2; end = 2 + static_cast<size_t>(count) * 2; stride = 2;
    } else if (destination || named(command,
        "LMPOP ZMPOP SINTERCARD ZINTERCARD ZUNION ZINTER ZDIFF")) {
        const size_t number = destination ? 2 : 1;
        long long count;
        if (arguments.size() <= number ||
            !parse_redis_integer(*arguments[number], count) || count < 1 ||
            static_cast<unsigned long long>(count) >
                arguments.size() - number - 1) {
            arguments.clear(); return false;
        }
        first = number + 1; end = first + static_cast<size_t>(count);
    } else if (named(command, "MSET MSETNX")) {
        if (arguments.size() % 2 != 1) {
            arguments.clear(); return false;
        }
        stride = 2;
    } else if (named(command, "BITOP")) first = 2;
    else if (named(command,
        "LCS RENAME RENAMENX COPY LMOVE RPOPLPUSH SMOVE ZRANGESTORE "
        "GEOSEARCHSTORE"))
        end = std::min(end, size_t(3));
    else if (!named(command, "WATCH MGET DEL EXISTS PFCOUNT PFMERGE "
        "SUNION SINTER SDIFF "
        "SUNIONSTORE SINTERSTORE SDIFFSTORE")) end = std::min(end, size_t(2));
    const auto* target = destination ? arguments[1] : nullptr;
    size_t output = 0;
    if (target) arguments[output++] = target;
    for (size_t i = first; i < end; i += stride)
        arguments[output++] = arguments[i];
    arguments.resize(output);
    return true;
}
} // namespace pkv
