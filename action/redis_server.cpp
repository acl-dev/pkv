//
// Created by shuxin zheng on 2023/8/2.
//

#include "stdafx.h"
#include "coder/redis_coder.h"
#include "coder/redis_object.h"

#include "redis_handler.h"
#include "redis_server.h"
#include "common/redis_integer.h"
#include <climits>

namespace pkv {

#define EQ  !strcasecmp

redis_server::redis_server(redis_handler &handler, const redis_object &obj)
: redis_command(handler, obj)
{}

struct server_handler {
    const char* cmd;
    bool (redis_server::*func) (redis_coder&);
};

static struct server_handler handlers[] = {
    { "DBSIZE",     &redis_server::dbsize   },
    { "TIME",       &redis_server::time     },
    { "SELECT",     &redis_server::select   },
    { "PING",       &redis_server::ping     },
    { "ECHO",       &redis_server::echo     },
    { "INFO",       &redis_server::info     },
    { "CONFIG",     &redis_server::config   },
    { "COMMAND",    &redis_server::command  },
    { "ACL",        &redis_server::acl      },
    { "AUTH",       &redis_server::auth     },
    { "HELLO",      &redis_server::hello    },
    { "CLIENT",     &redis_server::client   },
    { nullptr,      nullptr                 },
};

bool redis_server::exec(const char* cmd, redis_coder& result) {
    for (int i = 0; handlers[i].cmd != nullptr; i++) {
        if (EQ(cmd, handlers[i].cmd)) {
            return (this->*(handlers[i].func))(result);
        }
    }

    logger_error("Not support, cmd=%s", cmd);
    return false;
}

bool redis_server::select(redis_coder& result) {
    long long index;
    auto& reply = result.create_object();
    if (!parse_redis_integer(*obj_.argument(1), index)) {
        reply.set_error("ERR value is not an integer or out of range");
    } else if (index < INT_MIN || index > INT_MAX) {
        reply.set_error("ERR value is out of range, value must between -2147483648 and 2147483647");
    } else if (var_cfg_cluster_mode && index != 0) {
        reply.set_error("ERR SELECT is not allowed in cluster mode");
    } else if (index < 0 || static_cast<unsigned long long>(index) >= handler_.database_count()) {
        reply.set_error("ERR DB index is out of range");
    } else if (!handler_.select_database(static_cast<size_t>(index))) {
        reply.set_error("ERR storage operation failed");
    } else {
        reply.set_status("OK");
    }
    return true;
}

bool redis_server::ping(redis_coder& result) {
    if (handler_.client().protocol() == 2 && handler_.subscribed()) {
        auto& reply = result.create_object();
        reply.set_type(REDIS_OBJ_ARRAY);
        reply.create_child().set_string("pong");
        reply.create_child().set_string(obj_.size() == 1 ? "" :
            *obj_.argument(1));
    } else if (obj_.size() == 1)
        result.create_object().set_status("PONG");
    else result.create_object().set_string(*obj_.argument(1));
    return true;
}

bool redis_server::echo(redis_coder& result) {
    result.create_object().set_string(*obj_.argument(1));
    return true;
}

} // namespace pkv
