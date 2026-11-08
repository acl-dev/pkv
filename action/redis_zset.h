#pragma once
#include "redis_command.h"

namespace pkv {
class redis_zset : public redis_command {
public:
    redis_zset(redis_handler& handler, const redis_object& obj) :
        redis_command(handler, obj) {}
    bool exec(const char* cmd, redis_coder& result);
    bool pop(bool maximum, redis_coder& result);
private:
    bool algebra(const char* cmd, redis_coder& result);
    bool mpop(redis_coder& result);
    bool sample(redis_coder& result);
    bool add(bool increment, redis_coder& result);
    bool select(const char* cmd, redis_coder& result);
};
}
