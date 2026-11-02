#include "acl_internal.h"
#include "redis_glob.h"
#include <algorithm>
#include <stdexcept>
#include <limits>

namespace pkv {
acl_identity::acl_identity(std::string label, uint64_t number,
    std::shared_ptr<const acl_user> settings)
: name(std::move(label)), id(number) { publish(std::move(settings)); }
void acl_identity::publish(std::shared_ptr<const acl_user> settings) {
    const auto& root = settings->selectors.front();
    const bool open = root.all_keys && root.all_channels &&
        root.commands.size() == acl_catalog().size();
    std::atomic_store_explicit(&settings_, std::move(settings),
        std::memory_order_release);
    open_.store(open, std::memory_order_release);
}
std::shared_ptr<const acl_user> acl_identity::snapshot() const {
    return std::atomic_load_explicit(&settings_, std::memory_order_acquire);
}
acl_policy::acl_policy(const std::string& password) {
    (void) acl_lookup("get");
    auto settings = std::make_shared<acl_user>();
    settings->enabled = true; settings->nopass = password.empty();
    if (!password.empty()) settings->passwords.push_back(
        acl_password_hash(password));
    auto& root = settings->selectors.front();
    root.all_keys = true; root.all_channels = true; root.future_commands = true;
    for (const auto* entry : acl_catalog()) root.commands.insert(entry->name);
    std::shared_ptr<acl_identity> identity(new acl_identity("default",
        next_id_++, std::move(settings)));
    users_.emplace(identity->name, std::move(identity));
}
std::shared_ptr<acl_identity> acl_policy::find(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = users_.find(name);
    return it == users_.end() ? nullptr : it->second;
}
std::vector<std::shared_ptr<acl_identity>> acl_policy::users() const {
    std::vector<std::shared_ptr<acl_identity>> result;
    std::lock_guard<std::mutex> lock(mutex_);
    result.reserve(users_.size());
    for (const auto& entry : users_) result.push_back(entry.second);
    return result;
}
std::shared_ptr<acl_identity> acl_policy::authenticate(const std::string& name,
    const std::string& password) const {
    auto identity = find(name);
    if (!identity || identity->revoked()) return nullptr;
    const auto settings = identity->snapshot();
    if (!settings->enabled) return nullptr;
    if (settings->nopass) return identity;
    const auto hash = acl_password_hash(password);
    bool match = false;
    for (const auto& stored : settings->passwords)
        match = acl_secret_equal(stored, hash) || match;
    return match ? identity : nullptr;
}
bool acl_policy::default_login() const {
    const auto settings = find("default")->snapshot();
    return settings->enabled && settings->nopass;
}
std::string acl_policy::set(const std::string& name,
    const std::vector<const std::string*>& rules) {
    if (!acl_valid_token(name))
        return "ERR Usernames can't contain spaces or null characters";
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = users_.find(name);
    if (revision_ == std::numeric_limits<uint64_t>::max())
        return "ERR ACL policy revisions exhausted";
    auto staged = it == users_.end() ? std::make_shared<acl_user>() :
        std::make_shared<acl_user>(*it->second->snapshot());
    const auto error = acl_apply(*staged, rules);
    if (!error.empty()) return error;
    if (it != users_.end()) it->second->publish(std::move(staged));
    else {
        if (!next_id_) throw std::overflow_error("ACL identities exhausted");
        std::shared_ptr<acl_identity> identity(new acl_identity(name,
            next_id_, std::move(staged)));
        users_.emplace(name, std::move(identity)); ++next_id_;
    }
    ++revision_;
    return {};
}
std::vector<std::shared_ptr<acl_identity>> acl_policy::remove(
    const std::vector<const std::string*>& names) {
    std::vector<std::shared_ptr<acl_identity>> removed;
    for (const auto* name : names) if (*name == "default")
        throw std::invalid_argument("The 'default' user cannot be removed");
    std::lock_guard<std::mutex> lock(mutex_);
    if (revision_ == std::numeric_limits<uint64_t>::max())
        throw std::overflow_error("ACL policy revisions exhausted");
    removed.reserve(names.size());
    for (const auto* name : names) {
        const auto it = users_.find(*name);
        if (it != users_.end() && std::find(removed.begin(), removed.end(),
            it->second) == removed.end()) removed.push_back(it->second);
    }
    for (const auto& identity : removed) {
        identity->revoked_.store(true, std::memory_order_release);
        users_.erase(identity->name);
    }
    if (!removed.empty()) ++revision_;
    return removed;
}
namespace {
bool key_allowed(const acl_selector& selector, const std::string& key,
    unsigned access) {
    if (selector.all_keys) return true;
    for (const auto& pattern : selector.keys)
        if ((pattern.access & access) == access &&
            redis_glob_match(pattern.text, key)) return true;
    return false;
}
unsigned key_access(const std::string& flags) {
    unsigned access = 0;
    if (flags.find("access") != std::string::npos) access |= 1;
    if (flags.find("update") != std::string::npos ||
        flags.find("insert") != std::string::npos ||
        flags.find("delete") != std::string::npos) access |= 2;
    return access;
}
} // namespace
acl_decision acl_policy::check(const acl_identity& identity,
    const std::vector<const std::string*>& arguments) const {
    acl_decision result;
    if (identity.revoked()) {
        result.reason = acl_denial::revoked;
        if (!arguments.empty()) {
            result.object = command_lower(*arguments.front());
            if (acl_has_children(result.object) && arguments.size() > 1)
                result.object += '|' + command_lower(*arguments[1]);
        }
        return result;
    }
    if (arguments.empty()) {
        result.reason = acl_denial::command; return result;
    }
    auto name = command_lower(*arguments.front());
    if (name.find('|') != std::string::npos) {
        result.reason = acl_denial::command;
        result.object = name; return result;
    }
    if (acl_has_children(name) && arguments.size() > 1)
        name += '|' + command_lower(*arguments[1]);
    result.reason = acl_denial::command; result.object = name;
    const auto* entry = acl_lookup(name);
    if (!entry) return result;
    if (name == "auth" || name == "hello" || name == "reset" || name == "quit")
        return acl_decision();
    std::vector<command_key_argument> keys;
    const bool valid_keys = command_key_arguments(
        *entry, arguments, keys, true);
    const auto settings = identity.snapshot();
    const bool pattern = name == "psubscribe";
    const bool subscription = pattern || name == "subscribe" ||
        name == "ssubscribe";
    const size_t channel_end = subscription ? arguments.size() :
        (name == "publish" || name == "spublish") && arguments.size() > 1 ?
            2 : 1;
    size_t last_denied = 0;
    for (const auto& selector : settings->selectors) {
        bool command = selector.commands.count(name) != 0;
        const auto first = selector.first_arguments.find(name);
        if (!command && arguments.size() > 1 &&
            first != selector.first_arguments.end())
            command = first->second.count(command_lower(*arguments[1])) != 0;
        if (!command) continue;
        bool allowed = true;
        if (valid_keys) for (const auto& key : keys)
            if (!key_allowed(selector, *key.key, key_access(key.flags))) {
                const auto position = static_cast<size_t>(std::find(
                    arguments.begin(), arguments.end(), key.key) -
                    arguments.begin());
                if (result.reason != acl_denial::key ||
                    position > last_denied) {
                    result.reason = acl_denial::key; result.object = *key.key;
                    last_denied = position;
                }
                allowed = false; break;
            }
        if (allowed) for (size_t i = 1; i < channel_end; ++i)
            if (!acl_channel_allowed(selector, *arguments[i], pattern)) {
                if (result.reason != acl_denial::channel || i > last_denied) {
                    result.reason = acl_denial::channel;
                    result.object = *arguments[i]; last_denied = i;
                }
                allowed = false; break;
            }
        // Invalid key specifications cannot execute storage operations; their
        // ordinary command handler is responsible for the syntax error.
        if (allowed) return acl_decision();
    }
    return result;
}
bool acl_channel_allowed(const acl_selector& selector,
    const std::string& channel, bool pattern) {
    if (selector.all_channels) return true;
    for (const auto& allowed : selector.channels)
        if (pattern ? allowed == channel : redis_glob_match(allowed, channel))
            return true;
    return false;
}
} // namespace pkv
