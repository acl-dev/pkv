#pragma once
#include "redis_command.h"

namespace pkv {
class redis_stream : public redis_command {
public:
    using redis_command::redis_command;
    bool exec(const char* name, redis_coder& result);
};
} // namespace pkv
