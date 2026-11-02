#pragma once
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>
#include <utility>

namespace pkv {
struct acl_pattern {
    std::string text;
    unsigned access;
};
struct acl_selector {
    bool all_keys = false, all_channels = false, future_commands = false;
    std::set<std::string> commands;
    std::map<std::string, std::set<std::string>> first_arguments;
    std::vector<acl_pattern> keys;
    std::vector<std::string> channels, command_rules;
};
struct acl_user {
    bool enabled = false, nopass = false, sanitize = true;
    std::vector<std::string> passwords;
    std::vector<acl_selector> selectors{acl_selector()};
};
class acl_identity {
public:
    const std::string name;
    const uint64_t id;
    bool revoked() const { return revoked_.load(std::memory_order_acquire); }
    bool unrestricted() const { return open_.load(std::memory_order_acquire); }
    std::shared_ptr<const acl_user> snapshot() const;
private:
    friend class acl_policy;
    acl_identity(std::string label, uint64_t number,
        std::shared_ptr<const acl_user> settings);
    void publish(std::shared_ptr<const acl_user> settings);
    std::shared_ptr<const acl_user> settings_;
    std::atomic<bool> revoked_{false}, open_{false};
};
enum class acl_denial { none, command, key, channel, revoked };
struct acl_decision {
    acl_denial reason = acl_denial::none;
    std::string object;
    bool allowed() const { return reason == acl_denial::none; }
};
class acl_reload {
public:
    const std::vector<std::shared_ptr<acl_identity>>& removed() const {
        return removed_;
    }
private:
    friend class acl_policy;
    const void* owner_ = nullptr;
    uint64_t revision_ = 0, next_id_ = 0;
    std::map<std::string, std::shared_ptr<acl_identity>> table_;
    std::vector<std::pair<std::shared_ptr<acl_identity>,
        std::shared_ptr<const acl_user>>> updates_;
    std::vector<std::shared_ptr<acl_identity>> removed_;
};
class acl_policy {
public:
    explicit acl_policy(const std::string& password = {});
    std::shared_ptr<acl_identity> find(const std::string& name) const;
    std::vector<std::shared_ptr<acl_identity>> users() const;
    std::shared_ptr<acl_identity> authenticate(const std::string& name,
        const std::string& password) const;
    bool default_login() const;
    std::string describe() const;
    std::string prepare(const std::string& text, acl_reload& reload) const;
    std::string commit(acl_reload& reload);
    // Returns an error without publishing any part of a failed rule list.
    std::string set(const std::string& name,
        const std::vector<const std::string*>& rules);
    std::vector<std::shared_ptr<acl_identity>> remove(
        const std::vector<const std::string*>& names);
    acl_decision check(const acl_identity& identity,
        const std::vector<const std::string*>& arguments) const;
private:
    mutable std::mutex mutex_;
    uint64_t next_id_ = 1, revision_ = 0;
    std::map<std::string, std::shared_ptr<acl_identity>> users_;
};
std::string acl_password_hash(const std::string& password);
bool acl_secret_equal(const std::string& a, const std::string& b);
bool acl_valid_token(const std::string& text);
std::string acl_safe_token(const std::string& text);
std::vector<std::string> acl_categories();
std::vector<std::string> acl_commands(const std::string& category = {});
std::string acl_selector_commands(const acl_selector& selector);
std::string acl_selector_keys(const acl_selector& selector);
std::string acl_selector_channels(const acl_selector& selector);
std::string acl_describe(const acl_identity& identity, const acl_user& user);
bool acl_channel_allowed(const acl_selector& selector,
    const std::string& channel, bool pattern);
// Private parser entry point shared by publication and native tests.
std::string acl_apply(acl_user& user,
    const std::vector<const std::string*>& rules);
} // namespace pkv
