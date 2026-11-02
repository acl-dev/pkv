#pragma once
#include "redis_command.h"

namespace pkv {

class redis_list : public redis_command {
public:
    redis_list(redis_handler& handler, const redis_object& obj)
        : redis_command(handler, obj) {}
    bool exec(const char* cmd, redis_coder& result);
private:
    bool move(bool legacy, redis_coder& result);
    bool blocking(const char* command, redis_coder& result);
    bool positions(redis_coder& result);
    bool mpop(redis_coder& result);
};

} // namespace pkv
