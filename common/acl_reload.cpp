#include "acl_internal.h"
#include <limits>

namespace pkv {
namespace {
using settings_table = std::map<std::string, std::shared_ptr<const acl_user>>;
std::string parse(const std::string& text, settings_table& settings) {
    size_t start = 0, line = 0;
    while (start < text.size()) {
        ++line;
        const auto end = text.find('\n', start);
        const auto row = text.substr(start, end - start);
        const auto prefix = "ERR ACL file line " + std::to_string(line) + ": ";
        std::vector<std::string> words;
        if (row.find('\0') != std::string::npos || !acl_split(row, words))
            return prefix + "invalid byte or quoting";
        if (!words.empty()) {
            if (words.size() < 2 || words[0] != "user")
                return prefix + "expected user followed by a username";
            if (!acl_valid_token(words[1]))
                return prefix + "invalid username";
            if (settings.count(words[1])) return prefix + "duplicate user";
            auto user = std::make_shared<acl_user>();
            std::vector<const std::string*> rules;
            for (size_t i = 2; i < words.size(); ++i)
                rules.push_back(&words[i]);
            if (!acl_apply(*user, rules).empty())
                return prefix + "invalid ACL rule";
            settings.emplace(words[1], std::move(user));
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    if (!settings.count("default")) {
        auto user = std::make_shared<acl_user>();
        user->enabled = true; user->nopass = true;
        auto& root = user->selectors.front();
        root.all_keys = true; root.all_channels = true;
        root.future_commands = true;
        for (const auto* entry : acl_catalog())
            root.commands.insert(entry->name);
        settings.emplace("default", std::move(user));
    }
    return {};
}
} // namespace
std::string acl_policy::describe() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string text;
    for (const auto& entry : users_)
        text += acl_describe(*entry.second, *entry.second->snapshot()) + '\n';
    return text;
}
std::string acl_policy::prepare(const std::string& text,
    acl_reload& reload) const {
    settings_table settings;
    const auto error = parse(text, settings);
    if (!error.empty()) return error;
    acl_reload staged;
    std::lock_guard<std::mutex> lock(mutex_);
    if (revision_ == std::numeric_limits<uint64_t>::max())
        return "ERR ACL policy revisions exhausted";
    staged.revision_ = revision_; staged.next_id_ = next_id_;
    staged.updates_.reserve(settings.size());
    staged.removed_.reserve(users_.size());
    for (const auto& entry : settings) {
        const auto prior = users_.find(entry.first);
        std::shared_ptr<acl_identity> identity;
        if (prior != users_.end()) {
            identity = prior->second;
            staged.updates_.emplace_back(identity, entry.second);
        } else {
            if (!staged.next_id_) return "ERR ACL identities exhausted";
            identity.reset(new acl_identity(entry.first, staged.next_id_++,
                entry.second));
        }
        staged.table_.emplace(entry.first, std::move(identity));
    }
    for (const auto& entry : users_)
        if (!settings.count(entry.first))
            staged.removed_.push_back(entry.second);
    staged.owner_ = this;
    reload = std::move(staged);
    return {};
}
std::string acl_policy::commit(acl_reload& reload) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (reload.owner_ != this || reload.revision_ != revision_ ||
        revision_ == std::numeric_limits<uint64_t>::max())
        return "ERR ACL policy changed while preparing reload";
    // Everything that allocates has completed. Server callers hold exclusive
    // execution admission; individual retained identities publish atomically.
    for (const auto& update : reload.updates_)
        update.first->publish(update.second);
    for (const auto& removed : reload.removed_)
        removed->revoked_.store(true, std::memory_order_release);
    users_.swap(reload.table_);
    next_id_ = reload.next_id_; ++revision_;
    reload.owner_ = nullptr;
    return {};
}
} // namespace pkv
