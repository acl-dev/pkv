#pragma once
#include "redis_command.h"

namespace pkv {
class redis_geo : public redis_command {
public:
    redis_geo(redis_handler& handler, const redis_object& object)
    : redis_command(handler, object) {}
    bool exec(const char* command, redis_coder& result);
private:
    bool search(const char* command, redis_coder& result);
    bool add(redis_coder& result);
    bool read(const char* command, redis_coder& result);
};
} // namespace pkv
