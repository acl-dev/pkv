#include "transaction_database.h"
#include "redis_integer.h"
#include <climits>
#include <cstring>
#include <strings.h>

namespace pkv {
namespace {
bool named(const std::string& value, const char* name) {
    return value.size() == std::strlen(name) &&
        !strncasecmp(value.data(), name, value.size());
}
bool valid_database(const std::string& text, size_t count, bool cluster,
    size_t& out) {
    long long value;
    if (!parse_redis_integer(text, value) || value < 0 || value > INT_MAX ||
        static_cast<unsigned long long>(value) >= count ||
        (cluster && value != 0)) return false;
    out = static_cast<size_t>(value); return true;
}
bool copy_database(const std::vector<std::string>& args, size_t count,
    bool cluster, size_t current, size_t& out) {
    size_t destination = current;
    for (size_t i = 3; i < args.size(); ++i) {
        if (named(args[i], "REPLACE")) continue;
        if (!named(args[i], "DB") || i + 1 == args.size() ||
            !valid_database(args[++i], count, cluster, destination))
            return false;
    }
    out = destination; return true;
}
} // namespace
bool transaction_database(
    const std::vector<std::vector<std::string>>& commands, size_t initial,
    size_t count, bool cluster, size_t& selected,
    std::vector<const std::string*>& legacy_writers,
    const std::function<bool(const std::vector<std::string>&)>& allowed) {
    size_t current = initial, target = 0;
    bool found = false;
    legacy_writers.clear();
    for (const auto& args : commands) {
        if (args.empty()) return false;
        if (allowed && !allowed(args)) continue;
        const auto& name = args.front();
        if (named(name, "MOVE") || named(name, "CLUSTER") ||
            named(name, "INFO") ||
            named(name, "ACL") || named(name, "PUBSUB") ||
            named(name, "SUBSCRIBE") || named(name, "UNSUBSCRIBE") ||
            named(name, "PSUBSCRIBE") || named(name, "PUNSUBSCRIBE") ||
            named(name, "SSUBSCRIBE") || named(name, "SUNSUBSCRIBE") ||
            named(name, "PUBLISH") || named(name, "SPUBLISH") ||
            named(name, "AUTH") || named(name, "HELLO")) return false;
        if (named(name, "CONFIG") && args.size() > 1 &&
            named(args[1], "SET")) return false;
        if (named(name, "SELECT")) {
            if (args.size() == 2)
                valid_database(args[1], count, cluster, current);
            continue;
        }
        if (named(name, "TIME") || named(name, "PING") ||
            named(name, "ECHO") ||
            named(name, "CONFIG") || named(name, "COMMAND") ||
            named(name, "CLIENT") || named(name, "HELLO") ||
            named(name, "AUTH") ||
            named(name, "UNWATCH")) continue;
        if (found && current != target) return false;
        target = current; found = true;
        if (named(name, "COPY")) {
            size_t destination;
            if (copy_database(args, count, cluster, current, destination) &&
                destination != target) return false;
        }
        // Opaque legacy views can still require byte-preserving transfers.
        // Source workspace admission checks the decoded import capability.
        if (args.size() > 1 && (named(name, "COPY") ||
            named(name, "RENAME") || named(name, "RENAMENX")))
            legacy_writers.push_back(&args[1]);
    }
    if (found) selected = target;
    return found;
}
} // namespace pkv
