#pragma once
#include "redis_command.h"
namespace pkv {
class redis_blocking_pop : public redis_command {
public:
    redis_blocking_pop(redis_handler& handler, const redis_object& object)
    : redis_command(handler, object) {}
    bool exec(const char* command, redis_coder& result);
private:
    bool run(const char* command, redis_coder& result);
};
} // namespace pkv
