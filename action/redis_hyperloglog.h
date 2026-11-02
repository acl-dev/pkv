#pragma once
#include "redis_command.h"

namespace pkv {
class redis_hyperloglog : public redis_command {
public:
    redis_hyperloglog(redis_handler& handler, const redis_object& object)
    : redis_command(handler, object) {}
    bool exec(const char* command, redis_coder& result);
};
} // namespace pkv
