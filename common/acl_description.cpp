#include "acl_internal.h"

namespace pkv {
namespace {
void append(std::string& out, const std::string& token) {
    if (!out.empty()) out += ' ';
    out += token;
}
} // namespace
std::string acl_selector_commands(const acl_selector& selector) {
    std::string out = selector.future_commands ? "+@all" : "-@all";
    for (const auto& rule : selector.command_rules)
        append(out, acl_quote(rule));
    return out;
}
std::string acl_selector_keys(const acl_selector& selector) {
    if (selector.all_keys) return "~*";
    for (const auto& pattern : selector.keys)
        if (pattern.access == 3 && pattern.text == "*") return "~*";
    std::string out;
    for (const auto& pattern : selector.keys) {
        const std::string prefix = pattern.access == 3 ? "~" :
            pattern.access == 1 ? "%R~" : "%W~";
        append(out, acl_quote(prefix + pattern.text));
    }
    return out;
}
std::string acl_selector_channels(const acl_selector& selector) {
    if (selector.all_channels) return "&*";
    std::string out;
    for (const auto& channel : selector.channels)
        append(out, acl_quote('&' + channel));
    return out;
}
std::string acl_describe(const acl_identity& identity, const acl_user& user) {
    std::string out = "user " + acl_quote(identity.name);
    append(out, user.enabled ? "on" : "off");
    if (user.nopass) append(out, "nopass");
    if (!user.sanitize) append(out, "skip-sanitize-payload");
    for (const auto& password : user.passwords) append(out, '#' + password);
    const auto& root = user.selectors.front();
    const auto keys = acl_selector_keys(root);
    if (!keys.empty()) append(out, keys);
    const auto channels = acl_selector_channels(root);
    append(out, channels.empty() ? "resetchannels" : channels);
    append(out, acl_selector_commands(root));
    for (size_t i = 1; i < user.selectors.size(); ++i) {
        const auto& selector = user.selectors[i];
        std::string text = acl_selector_commands(selector);
        const auto selected_keys = acl_selector_keys(selector);
        const auto selected_channels = acl_selector_channels(selector);
        if (!selected_keys.empty()) append(text, selected_keys);
        if (!selected_channels.empty()) append(text, selected_channels);
        const auto wrapped = '(' + text + ')';
        append(out, text.find_first_of("\"'\\") == std::string::npos ?
            wrapped : acl_quote(wrapped));
    }
    return out;
}
} // namespace pkv
