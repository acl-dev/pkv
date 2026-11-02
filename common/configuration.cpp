#include "configuration.h"
#include "redis_glob.h"
#include "redis_integer.h"
#include <algorithm>
#include <climits>
#include <set>

namespace pkv {
const std::vector<std::string>& config_names() {
    static const std::vector<std::string> names{"aclfile", "acllog-max-len",
        "appendonly", "cluster-enabled", "databases", "replica-read-only",
        "requirepass", "save", "slave-read-only"};
    return names;
}
std::string config_canonical(const std::string& name) {
    auto value = name;
    for (auto& ch : value) if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
    return value;
}
bool config_known(const std::string& name) {
    const auto& names = config_names();
    const auto canonical = config_canonical(name);
    return std::find(names.begin(), names.end(), canonical) != names.end();
}
bool config_pattern(const std::string& pattern) {
    const auto prefix = pattern.substr(0, pattern.find('\0'));
    return prefix.find_first_of("[*?") != std::string::npos;
}
bool config_matches(const std::string& pattern, const std::string& name) {
    if (!config_pattern(pattern)) return config_canonical(pattern) == name;
    return redis_glob_match(config_canonical(pattern.substr(0,
        pattern.find('\0'))), name);
}
namespace {
std::string safe(const std::string& value) {
    auto result = value.substr(0, value.find('\0'));
    for (auto& ch : result) if (ch == '\r' || ch == '\n') ch = ' ';
    return result;
}
std::string failure(const std::string& name, const char* reason) {
    return "ERR CONFIG SET failed (possibly related to argument "
                    "'" +
        safe(name) + "') - " + reason;
}
bool immutable(const std::string& name) {
    return name == "aclfile" || name == "databases" ||
        name == "cluster-enabled";
}
} // namespace
std::string config_prepare(const std::vector<const std::string*>& pairs,
    config_change& change) {
    if (pairs.empty() || pairs.size() % 2) return "ERR syntax error";
    std::set<std::string> seen;
    for (size_t i = 0; i < pairs.size(); i += 2) {
        const auto& name = *pairs[i];
        if (!config_known(name)) return "ERR Unknown option or number of "
            "arguments for CONFIG SET - '" + safe(name) + "'";
        if (immutable(config_canonical(name)))
            return failure(name, "can't set immutable config");
        if (!seen.insert(config_canonical(name)).second)
            return failure(name, "duplicate parameter");
    }
    config_change prepared;
    for (size_t i = 0; i < pairs.size(); i += 2) {
        const auto name = config_canonical(*pairs[i]);
        const auto& value = *pairs[i + 1];
        if (name == "requirepass") {
            prepared.password = true; prepared.value = value;
        } else if (name == "acllog-max-len") {
            long long number;
            if (!parse_redis_integer(value, number)) return failure(name,
                "argument couldn't be parsed into an integer");
            if (number < 0 || static_cast<unsigned long long>(number) >
                static_cast<unsigned long long>(LONG_MAX))
                return "ERR CONFIG SET failed (possibly related to argument "
                    "'" +
                    safe(name) + "') - argument must be between 0 and " +
                    std::to_string(LONG_MAX) + " inclusive";
            prepared.audit = true; prepared.limit = static_cast<size_t>(number);
        } else if (name == "appendonly" || name == "replica-read-only" ||
            name == "slave-read-only") {
            auto boolean = value.substr(0, value.find('\0'));
            for (auto& ch : boolean) if (ch >= 'A' && ch <= 'Z')
                ch += 'a' - 'A';
            if (boolean != "yes" && boolean != "no")
                return failure(name, "argument must be 'yes' or 'no'");
            if (name != "appendonly") {
                prepared.replica_policy = true;
                prepared.replica_read_only = boolean == "yes";
            } else if (boolean == "yes") return failure(name,
                "append-only persistence is not implemented");
        } else if (name == "save" && !value.empty())
            return failure(name, "RDB snapshot scheduling is not implemented");
    }
    change = std::move(prepared);
    return {};
}
} // namespace pkv
