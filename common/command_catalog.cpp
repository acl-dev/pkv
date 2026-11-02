#include "command_catalog.h"
#include "redis_integer.h"
#include "geo_radius.h"
#include <strings.h>
#include <cstring>
#include <climits>
#include <algorithm>

namespace pkv {
namespace {
#include "command_catalog_data.inc"

std::string variable_flags(const std::string& name,
    const std::vector<const std::string*>& args) {
    if (name == "delex") {
        for (size_t i = 2; i < args.size(); ++i) {
            const auto option = command_lower(*args[i]);
            if (option == "ifeq" || option == "ifne" || option == "ifdeq" ||
                option == "ifdne") return "RW delete";
        }
        return "RM delete";
    }
    if (name == "set") {
        bool access = false, conditional = false;
        for (size_t i = 3; i < args.size(); ++i) {
            const auto option = command_lower(*args[i]);
            access = access || option == "get";
            conditional = conditional || option == "ifeq" || option == "ifne"
                || option == "ifdeq" || option == "ifdne";
        }
        if (access) return "RW access update";
        return conditional ? "RW update" : "OW update";
    }
    for (size_t i = 2; i < args.size(); ++i) {
        const auto option = command_lower(*args[i]);
        const auto remaining = args.size() - i - 1;
        if (option == "get" && remaining >= 2) i += 2;
        else if (option == "overflow" && remaining >= 1) ++i;
        else return "RW access update";
    }
    return "RO access";
}
} // namespace

std::vector<const command_catalog_entry*> command_catalog_entries() {
    std::vector<const command_catalog_entry*> entries;
    entries.reserve(sizeof(catalog) / sizeof(catalog[0]));
    for (const auto& entry : catalog) entries.push_back(&entry);
    return entries;
}
std::string command_lower(std::string value) {
    for (char& byte : value)
        if (byte >= 'A' && byte <= 'Z') byte += 'a' - 'A';
    return value;
}
const command_catalog_entry* command_catalog(const std::string& name) {
    for (const auto& entry : catalog) {
        if (name.size() != std::strlen(entry.name)) continue;
        size_t i = 0;
        for (; i < name.size(); ++i) {
            char byte = name[i];
            if (byte >= 'A' && byte <= 'Z') byte += 'a' - 'A';
            if (byte != entry.name[i]) break;
        }
        if (i == name.size()) return &entry;
    }
    return nullptr;
}
std::vector<std::string> command_subcommands(const std::string& name) {
    const auto prefix = command_lower(name) + '|';
    std::vector<std::string> names;
    for (const auto& entry : catalog)
        if (!std::strncmp(entry.name, prefix.data(), prefix.size()))
            names.push_back(entry.name);
    return names;
}
bool command_key_arguments(const command_catalog_entry& entry,
    const std::vector<const std::string*>& args,
    std::vector<command_key_argument>& out, bool admission) {
    out.clear();
    for (size_t r = 0; r < entry.key_count; ++r) {
        const auto& rule = entry.keys[r];
        if (std::strstr(rule.flags, "not_key")) continue;
        if (rule.step <= 0 || (!rule.keyword && rule.begin < 0)) return false;
        size_t first = static_cast<size_t>(rule.begin), last;
        if (rule.keyword) {
            if (args.size() > static_cast<size_t>(LLONG_MAX)) return false;
            const auto size = static_cast<long long>(args.size());
            auto index = rule.begin > 0 ? static_cast<long long>(rule.begin) :
                size + rule.begin;
            first = 0;
            while (index >= 1 && index < size) {
                if ((rule.begin > 0 && index == size - 1) ||
                    (rule.begin <= 0 && index == 1)) break;
                if (!strcasecmp(args[size_t(index)]->c_str(), rule.keyword)) {
                    first = size_t(index + 1); break;
                }
                index += rule.begin > 0 ? 1 : -1;
            }
            if (!first) continue;
        }
        if (first >= args.size()) return false;
        if (rule.counted) {
            const auto index = first + static_cast<size_t>(rule.last_or_count);
            long long count;
            if (index >= args.size() || rule.first < 0 ||
                !parse_redis_integer(*args[index], count) || count < 1)
                return false;
            first += static_cast<size_t>(rule.first);
            if (first >= args.size() ||
                static_cast<unsigned long long>(count) >
                    (args.size() - first + rule.step - 1) / rule.step)
                return false;
            last = first + (static_cast<size_t>(count) - 1) * rule.step;
        } else {
            if (rule.last_or_count < 0) {
                const auto offset = static_cast<size_t>(-rule.last_or_count);
                if (offset > args.size()) return false;
                last = args.size() - offset;
            } else last = first + static_cast<size_t>(rule.last_or_count);
            if (last < first || last >= args.size()) return false;
        }
        auto flags = std::string(rule.flags);
        if (!std::strcmp(entry.name, "set") ||
            !std::strcmp(entry.name, "delex") ||
            !std::strcmp(entry.name, "bitfield"))
            flags = variable_flags(entry.name, args);
        for (size_t i = first; i <= last; i += rule.step)
            out.push_back({args[i], flags});
    }
    if (admission) {
        const auto mode = radius_mode(entry.name);
        if (mode != geo_radius_mode::none && !radius_readonly(mode)) {
            const auto target = radius_destination(args);
            if (target && std::none_of(out.begin(), out.end(),
                [&](const command_key_argument& key) {
                return key.key == args[target] && key.flags == "OW update";
            })) out.push_back({args[target], "OW update"});
        }
    }
    return true;
}
} // namespace pkv
