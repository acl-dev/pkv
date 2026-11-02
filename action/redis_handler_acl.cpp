#include "stdafx.h"
#include "redis_handler.h"
#include "redis_service.h"
#include "common/command_catalog.h"
#include "common/client_description.h"
#include "common/execution_gate.h"

namespace pkv {
bool redis_handler::acl_command_valid(const redis_object& object) {
    const auto name = command_lower(*object.argument(1));
    const auto* entry = command_catalog("acl|" + name);
    if (entry && ((entry->arity > 0 &&
        size_t(entry->arity) != object.size()) || (entry->arity < 0 &&
        size_t(-entry->arity) > object.size()))) {
        transaction_.fail();
        builder_.create_object().set_error("ERR wrong number of arguments "
            "for '" + std::string(entry->name) + "' command"); return false;
    }
    if (!entry) {
        transaction_.fail();
        builder_.create_object().set_error("ERR unknown subcommand '" +
            acl_safe_token(*object.argument(1)) +
            "'. Try ACL HELP."); return false;
    }
    return true;
}
acl_audit& redis_handler::audit() { return service_.audit(); }
acl_policy& redis_handler::permissions() { return service_.permissions(); }
bool redis_handler::acl_permit(const redis_object& object, redis_coder& result,
    bool queued) {
    if (!identity_->revoked() && identity_->unrestricted()) return true;
    const auto* name = object.argument(0);
    if (name && (*name == "AUTH" || *name == "HELLO" || *name == "RESET" ||
        *name == "QUIT" || !strcasecmp(name->c_str(), "AUTH") ||
        !strcasecmp(name->c_str(), "HELLO") ||
        !strcasecmp(name->c_str(), "RESET") ||
        !strcasecmp(name->c_str(), "QUIT"))) return true;
    std::vector<const std::string*> arguments;
    arguments.reserve(object.size());
    for (size_t i = 0; i < object.size(); ++i)
        arguments.push_back(object.argument(i));
    const auto decision = permissions().check(*identity_, arguments);
    if (decision.allowed()) return true;
    acl_record(decision.reason == acl_denial::key ? "key" :
        decision.reason == acl_denial::channel ? "channel" : "command",
        decision.object, identity_->name);
    std::string error;
    if (queued) error = "NOPERM ACLs rules changed between the moment the "
        "transaction was accumulated and the EXEC call. This command is no "
        "longer allowed for the following reason: " +
        std::string(decision.reason == acl_denial::key ?
            "no permission to touch the specified keys" :
            decision.reason == acl_denial::channel ?
            "no permission to access one of the channels used as arguments" :
            "no permission to execute the command or subcommand");
    else if (decision.reason == acl_denial::key)
        error = "NOPERM No permissions to access a key";
    else if (decision.reason == acl_denial::channel)
        error = "NOPERM No permissions to access a channel";
    else error = "NOPERM User " + acl_safe_token(identity_->name) +
        " has no permissions to run the '" + acl_safe_token(decision.object) +
        "' command";
    result.create_object().set_error(error);
    return false;
}
void redis_handler::acl_record(const char* reason, const std::string& object,
    const std::string& username) noexcept {
    statistics_->denied(!std::strcmp(reason, "auth") ? 0 :
        !std::strcmp(reason, "key") ? 2 :
        !std::strcmp(reason, "channel") ? 3 : 1);
    auto& audit = service_.audit();
    if (!audit.enabled()) { audit.trim_disabled(); return; }
    try {
        acl_event event;
        event.reason = reason; event.object = object; event.username = username;
        event.context = transaction_.active() || executing_ ? "multi" :
            "toplevel";
        event.updated = execution_time_ms();
        const auto connection = service_.clients().find(client().id());
        if (connection) append_client_description(event.client_info, connection,
            event.updated);
        if (!event.client_info.empty() && event.client_info.back() == '\n')
            event.client_info.pop_back();
        audit.record(event);
    } catch (...) { audit.lost(); }
}
} // namespace pkv
