#include "acl_internal.h"
#include <unordered_map>
#include <unordered_set>

namespace pkv {
namespace {
struct command_index {
    std::unordered_map<std::string, const command_catalog_entry*> commands;
    std::unordered_set<std::string> parents;
    command_index() {
        commands.reserve(acl_catalog().size());
        for (const auto* entry : acl_catalog()) {
            const std::string name(entry->name);
            commands.emplace(name, entry);
            const auto split = name.find('|');
            if (split != std::string::npos)
                parents.insert(name.substr(0, split));
        }
    }
};
const command_index& index() {
    static const command_index table;
    return table;
}
} // namespace
const command_catalog_entry* acl_lookup(const std::string& lower_name) {
    const auto& commands = index().commands;
    const auto found = commands.find(lower_name);
    return found == commands.end() ? nullptr : found->second;
}
bool acl_has_children(const std::string& lower_name) {
    return index().parents.count(lower_name) != 0;
}
} // namespace pkv
