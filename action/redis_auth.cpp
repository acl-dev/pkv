#include "stdafx.h"
#include "redis_server.h"
#include "redis_handler.h"
#include "redis_service.h"

namespace pkv {
bool redis_server::auth(redis_coder& result) {
    auto& reply = result.create_object();
    if (obj_.size() > 3) {
        reply.set_error("ERR syntax error"); return true;
    }
    auto& policy = handler_.permissions();
    if (obj_.size() == 2 && policy.find("default")->snapshot()->nopass) {
        reply.set_error("ERR AUTH <password> called without any password "
            "configured for the default user. Are you sure your "
            "configuration is correct?"); return true;
    }
    const auto& password = *obj_.argument(obj_.size() - 1);
    const std::string default_user("default");
    const auto& user = obj_.size() == 3 ? *obj_.argument(1) : default_user;
    auto identity = policy.authenticate(user, password);
    if (!identity) {
        handler_.acl_record("auth", *obj_.argument(0), user);
        reply.set_error("WRONGPASS invalid username-password pair or user "
            "is disabled."); return true;
    }
    if (handler_.client().login(identity->name, identity->id) !=
        client_state::status::ok) {
        reply.set_error("OOM client metadata allocation failed"); return true;
    }
    handler_.identity(std::move(identity));
    reply.set_status("OK");
    return true;
}
} // namespace pkv
