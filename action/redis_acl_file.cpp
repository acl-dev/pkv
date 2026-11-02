#include "stdafx.h"
#include "redis_server.h"
#include "redis_handler.h"
#include "redis_service.h"
#include "common/acl_file.h"
#include <set>

namespace pkv {
bool redis_server::acl_file(redis_object& reply, bool save) {
    const auto& path = handler_.get_service().acl_file();
    if (path.empty()) {
        reply.set_error("ERR This Redis instance is not configured to use an "
            "ACL file. You may want to specify users via the ACL SETUSER "
            "command and then issue a CONFIG REWRITE (assuming you have a "
            "Redis configuration file set) in order to store users in the "
            "Redis configuration."); return true;
    }
    auto& policy = handler_.permissions();
    if (save) {
        const auto result = acl_save_file(path, policy.describe());
        if (result.ok()) reply.set_status("OK");
        else {
            logger_error("ACL SAVE: %s, replacement visible=%d",
                result.error.c_str(), int(result.replaced));
            reply.set_error("ERR There was an error trying to save the ACLs. "
                "Please check the server logs for more information");
        }
        return true;
    }
    std::string text;
    auto error = acl_read_file(path, text);
    acl_reload reload;
    if (error.empty()) error = policy.prepare(text, reload);
    if (!error.empty()) { reply.set_error(error); return true; }
    std::set<uint64_t> removed;
    for (const auto& identity : reload.removed()) removed.insert(identity->id);
    std::vector<client_registry::connection> targets;
    for (const auto& connection : handler_.get_service().clients().snapshot(
        false)) {
        client_snapshot metadata;
        if (connection->read_metadata(metadata) &&
            removed.count(metadata.user_id))
            targets.push_back(connection);
    }
    const auto previous = policy.users();
    reply.set_status("OK");
    error = policy.commit(reload);
    if (!error.empty()) { reply.set_error(error); return true; }
    for (const auto& user : previous)
        if (!user->revoked()) handler_.config_service().pubsub().
            validate_permissions(user, handler_.client().id());
    const auto self = handler_.client().id();
    for (const auto& target : targets)
        target->request_stop(target->id() == self);
    return true;
}
} // namespace pkv
