#pragma once
#include "redis_command.h"

namespace pkv {
class redis_scan : public redis_command {
public:
    redis_scan(redis_handler& handler, const redis_object& obj) : redis_command(handler, obj) {}
    bool keys(redis_coder& result);
    bool random_key(redis_coder& result);
    bool exec(const char* cmd, redis_coder& result);
};
}
