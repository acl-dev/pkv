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

typedef std::function<bool(redis_handler&, const char* cmd,
    const redis_object&, redis_coder& result)> redis_handler_t;

class redis_service {
public:
    redis_service();
    ~redis_service() = default;

    void init();

    redis_handler_t* get_handler(const char* cmd);

private:
    std::map<std::string, redis_handler_t> handlers_;

    static bool key_handler(redis_handler&, const char*,
        const redis_object&, redis_coder& result);
    static bool string_handler(redis_handler&, const char*,
        const redis_object&, redis_coder& result);
    static bool hash_handler(redis_handler&, const char*,
        const redis_object&, redis_coder& result);
    static bool server_handler(redis_handler&, const char*,
        const redis_object&, redis_coder& result);
};

} // namespace pkv