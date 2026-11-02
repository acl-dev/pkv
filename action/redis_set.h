#pragma once
#include "redis_command.h"

namespace pkv {

class redis_set : public redis_command {
public:
    redis_set(redis_handler& handler, const redis_object& obj) : redis_command(handler, obj) {}
    bool exec(const char* cmd, redis_coder& result);
private:
    bool sample(bool remove, redis_coder& result);
    bool intersection_size(redis_coder& result);
};

} // namespace pkv
