//
// Created by shuxin zheng on 2023/8/2.
//

#pragma once
#include "redis_command.h"

namespace pkv {

class redis_coder;

class redis_server : public redis_command {
public:
    redis_server(redis_handler &handler, const redis_object &obj);
    ~redis_server() override = default;

    bool exec(const char* cmd, redis_coder& result);

public:
    bool dbsize(redis_coder& result);
    bool time(redis_coder& result);
    bool select(redis_coder& result);
    bool ping(redis_coder& result);
    bool echo(redis_coder& result);
    bool info(redis_coder& result);
    bool config(redis_coder &result);
    bool command(redis_coder& result);
    bool client(redis_coder& result);
    bool client_kill(redis_object& reply);
    bool client_unblock(redis_object& reply);
    bool client_info(redis_object& reply, bool list);
    bool hello(redis_coder& result);
    bool auth(redis_coder& result);
    bool acl(redis_coder& result);
    bool acl_dryrun(redis_object& reply);
    bool acl_deluser(redis_object& reply);
    bool acl_log(redis_object& reply);
    bool acl_file(redis_object& reply, bool save);

private:
    bool config_get(redis_object& reply);
    bool config_set(redis_object& reply);
};

} // namespace pkv
