//
// Created by shuxin ¡¡¡¡zheng on 2023/8/18.
//

#pragma once
#include <map>
#include <string>
#include <functional>

namespace pkv {

class redis_handler;
class redis_object;
class redis_coder;

typedef std::function<bool(redis_handler&, const redis_object&, redis_coder& result)>
        redis_handler_t;

class redis_service {
public:
    redis_service();
    ~redis_service() = default;

    void init();

    redis_handler_t* get_handler(const char* cmd);

private:
    std::map<std::string, redis_handler_t> handlers_;

    static bool cmd_set(redis_handler&, const redis_object&, redis_coder& result);
    static bool cmd_get(redis_handler&, const redis_object&, redis_coder& result);
    static bool cmd_del(redis_handler&, const redis_object&, redis_coder& result);
    static bool cmd_type(redis_handler&, const redis_object&, redis_coder& result);

    static bool cmd_hset(redis_handler&, const redis_object&, redis_coder& result);
    static bool cmd_hget(redis_handler&, const redis_object&, redis_coder& result);
    static bool cmd_hdel(redis_handler&, const redis_object&, redis_coder& result);
    static bool cmd_hmset(redis_handler&, const redis_object&, redis_coder& result);
    static bool cmd_hmget(redis_handler&, const redis_object&, redis_coder& result);
    static bool cmd_hgetall(redis_handler&, const redis_object&, redis_coder& result);

    static bool cmd_config(redis_handler&, const redis_object&, redis_coder& result);
};

} // namespace pkv