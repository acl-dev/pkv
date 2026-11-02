#include "acl_internal.h"
#include <algorithm>
#include <cctype>
#include <sstream>

namespace pkv {
const std::vector<const command_catalog_entry*>& acl_catalog() {
    static const auto entries = command_catalog_entries();
    return entries;
}
bool acl_valid_token(const std::string& text) {
    for (unsigned char ch : text) if (!ch || std::isspace(ch)) return false;
    return true;
}
std::string acl_safe_token(const std::string& text) {
    std::string safe(text);
    for (auto& ch : safe) if (!ch || ch == '\r' || ch == '\n') ch = ' ';
    return safe;
}
namespace {
bool category_has(const command_catalog_entry& entry, const std::string& name) {
    std::istringstream input(entry.categories);
    std::string category;
    while (input >> category) if (category == '@' + name) return true;
    return false;
}
void remember(acl_selector& selector, const std::string& rule) {
    const auto operand = rule.substr(1);
    auto& rules = selector.command_rules;
    rules.erase(std::remove_if(rules.begin(), rules.end(),
        [&](const std::string& old) { return old.substr(1) == operand; }),
        rules.end());
    rules.push_back(rule);
}
void forget_first_arguments(acl_selector& selector, const std::string& name) {
    auto& rules = selector.command_rules;
    rules.erase(std::remove_if(rules.begin(), rules.end(),
        [&](const std::string& old) {
            const auto operand = old.substr(1);
            return operand.compare(0, name.size() + 1, name + '|') == 0 &&
                !command_catalog(operand);
        }), rules.end());
}
void change(acl_selector& selector, const std::string& name, bool allow) {
    for (const auto* entry : acl_catalog()) {
        const std::string command(entry->name);
        if (command != name && command.compare(0, name.size() + 1,
            name + '|')) continue;
        if (allow) selector.commands.insert(command);
        else selector.commands.erase(command);
        selector.first_arguments.erase(command);
        forget_first_arguments(selector, command);
    }
}
std::string command_rule(acl_selector& selector, const std::string& rule) {
    const bool allow = rule[0] == '+';
    const auto name = command_lower(rule.substr(1));
    if (name == "@all") {
        selector.commands.clear(); selector.first_arguments.clear();
        selector.future_commands = allow;
        selector.command_rules.clear();
        if (allow) for (const auto* entry : acl_catalog())
            selector.commands.insert(entry->name);
        return {};
    }
    if (!name.empty() && name[0] == '@') {
        const auto category = name.substr(1);
        const auto categories = acl_categories();
        if (std::find(categories.begin(), categories.end(), category) ==
            categories.end()) return "Unknown command or category name in ACL";
        for (const auto* entry : acl_catalog())
            if (category_has(*entry, category)) change(selector, entry->name,
                allow);
        remember(selector, rule.substr(0, 1) + name);
        return {};
    }
    if (command_catalog(name)) {
        change(selector, name, allow);
        remember(selector, rule.substr(0, 1) + name); return {};
    }
    const auto split = name.find('|');
    if (allow && split != std::string::npos) {
        const auto root = name.substr(0, split);
        if (name.find('|', split + 1) != std::string::npos)
            return "Allowing first-arg of a subcommand is not supported";
        if (split + 1 == name.size()) return "Syntax error";
        if (command_catalog(root) && command_subcommands(root).empty()) {
            selector.first_arguments[root].insert(name.substr(split + 1));
            remember(selector, "+" + name);
            return {};
        }
    }
    return "Unknown command or category name in ACL";
}
std::string pattern_rule(acl_selector& selector, const std::string& rule) {
    const bool channel = rule[0] == '&';
    if (channel && selector.all_channels) return "Adding a pattern after the "
        "* pattern (or the 'allchannels' flag) is not valid and does not "
        "have any effect. Try 'resetchannels' to start with an empty list "
        "of channels";
    if (!channel && selector.all_keys) return "Adding a pattern after the "
        "* pattern (or the 'allkeys' flag) is not valid and does not have "
        "any effect. Try 'resetkeys' to start with an empty list of patterns";
    size_t offset = 1;
    unsigned access = 3;
    if (rule[0] == '%') {
        access = 0;
        while (offset < rule.size() && rule[offset] != '~') {
            const auto ch = std::toupper(static_cast<unsigned char>(
                rule[offset++]));
            const unsigned bit = ch == 'R' ? 1 : ch == 'W' ? 2 : 0;
            if (!bit || (access & bit)) return "Syntax error";
            access |= bit;
        }
        if (!access || offset == rule.size()) return "Syntax error";
        ++offset;
    }
    const auto text = rule.substr(offset);
    if (!acl_valid_token(text)) return "Syntax error";
    if (channel) {
        if (std::find(selector.channels.begin(), selector.channels.end(),
            text) == selector.channels.end()) selector.channels.push_back(text);
    } else {
        for (auto& pattern : selector.keys) if (pattern.text == text) {
            pattern.access |= access; return {};
        }
        selector.keys.push_back({text, access});
    }
    return {};
}
bool valid_hash(const std::string& hash) {
    if (hash.size() != 64) return false;
    for (char ch : hash)
        if ((ch < '0' || ch > '9') && (ch < 'a' || ch > 'f')) return false;
    return true;
}
std::string password_rule(acl_user& user, const std::string& rule) {
    const auto op = rule[0];
    const auto text = rule.substr(1);
    if ((op == '#' || op == '!') && !valid_hash(text))
        return "The password hash must be exactly 64 characters and contain "
            "only lowercase hexadecimal characters";
    const auto hash = op == '>' || op == '<' ? acl_password_hash(text) : text;
    auto it = std::find(user.passwords.begin(), user.passwords.end(), hash);
    if (op == '<' || op == '!') {
        if (it == user.passwords.end()) return "The password you are trying "
            "to remove from the user does not exist";
        user.passwords.erase(it);
    } else {
        if (it == user.passwords.end()) user.passwords.push_back(hash);
        user.nopass = false;
    }
    return {};
}
std::string user_rule(acl_user& user, const std::string& rule) {
    const auto lower = command_lower(rule);
    if (rule.empty()) return {};
    if (lower == "on") user.enabled = true;
    else if (lower == "off") user.enabled = false;
    else if (lower == "sanitize-payload") user.sanitize = true;
    else if (lower == "skip-sanitize-payload") user.sanitize = false;
    else if (lower == "reset") user = acl_user();
    else if (lower == "nopass" || lower == "resetpass") {
        user.nopass = lower == "nopass"; user.passwords.clear();
    } else if (lower == "clearselectors") user.selectors.resize(1);
    else if (rule[0] == '>' || rule[0] == '<' || rule[0] == '#' ||
        rule[0] == '!') return password_rule(user, rule);
    else if (rule[0] == '(' && rule.back() == ')') {
        acl_selector selector;
        std::vector<std::string> parts;
        if (!acl_split(rule.substr(1, rule.size() - 2), parts))
            return "Syntax error";
        for (const auto& part : parts) {
            const auto error = acl_selector_rule(selector, part);
            if (!error.empty()) return error;
        }
        user.selectors.push_back(std::move(selector));
    } else return acl_selector_rule(user.selectors.front(), rule);
    return {};
}
} // namespace
std::string acl_selector_rule(acl_selector& selector, const std::string& rule) {
    if (rule.empty()) return "Syntax error";
    const auto lower = command_lower(rule);
    if (lower == "allkeys" || rule == "~*") {
        selector.all_keys = true; selector.keys.clear();
    } else if (lower == "resetkeys") {
        selector.all_keys = false; selector.keys.clear();
    } else if (lower == "allchannels" || rule == "&*") {
        selector.all_channels = true; selector.channels.clear();
    } else if (lower == "resetchannels") {
        selector.all_channels = false; selector.channels.clear();
    } else if (lower == "allcommands" || lower == "nocommands")
        return command_rule(selector, lower == "allcommands" ? "+@all" :
            "-@all");
    else if (rule[0] == '+' || rule[0] == '-') {
        if (!acl_valid_token(rule)) return "Syntax error";
        return command_rule(selector, rule);
    } else if (rule[0] == '~' || rule[0] == '%' || rule[0] == '&')
        return pattern_rule(selector, rule);
    else return "Syntax error";
    return {};
}
std::string acl_apply(acl_user& user,
    const std::vector<const std::string*>& rules) {
    std::vector<std::string> merged;
    for (size_t i = 0; i < rules.size(); ++i) {
        std::string rule(*rules[i]);
        if (!rule.empty() && rule[0] == '(' && rule.back() != ')') {
            const auto first = rule;
            while (rule.back() != ')' && ++i < rules.size())
                rule += ' ' + *rules[i];
            if (rule.back() != ')') return "ERR Unmatched parenthesis in acl "
                "selector starting at '" + acl_safe_token(first) + "'.";
        }
        merged.push_back(std::move(rule));
    }
    for (const auto& rule : merged) {
        const auto error = user_rule(user, rule);
        if (!error.empty()) return "ERR Error in ACL SETUSER modifier '" +
            acl_safe_token(rule) + "': " + error;
    }
    return {};
}
std::vector<std::string> acl_categories() {
    std::set<std::string> unique;
    for (const auto* entry : acl_catalog()) {
        std::istringstream input(entry->categories);
        std::string name;
        while (input >> name) unique.insert(name.substr(1));
    }
    return {unique.begin(), unique.end()};
}
std::vector<std::string> acl_commands(const std::string& category) {
    std::vector<std::string> commands;
    const auto lower = command_lower(category);
    for (const auto* entry : acl_catalog())
        if (lower.empty() || category_has(*entry, lower))
            commands.push_back(entry->name);
    return commands;
}
} // namespace pkv
