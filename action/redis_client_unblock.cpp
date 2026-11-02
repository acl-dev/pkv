#include "stdafx.h"
#include "redis_server.h"
#include "redis_handler.h"
#include "redis_service.h"
#include "common/redis_integer.h"
namespace pkv {
bool redis_server::client_unblock(redis_object& reply) {
    if (obj_.size() > 4) {
        reply.set_error("ERR unknown subcommand or wrong number of arguments "
            "for '" + *obj_.argument(1) + "'. Try CLIENT HELP.");
        return true;
    }
    bool error = false;
    if (obj_.size() == 4) {
        const auto* reason = obj_.argument(3)->c_str();
        error = !strcasecmp(reason, "ERROR");
        if (!error && strcasecmp(reason, "TIMEOUT")) {
            reply.set_error(
                "ERR CLIENT UNBLOCK reason should be TIMEOUT or ERROR");
            return true;
        }
    }
    long long id;
    if (!parse_redis_integer(*obj_.argument(2), id)) {
        reply.set_error("ERR value is not an integer or out of range");
        return true;
    }
    const auto target = handler_.config_service().clients().find(id);
    reply.set_number(target && target->unblock(error) ? 1 : 0);
    return true;
}
} // namespace pkv
