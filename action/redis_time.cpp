#include "stdafx.h"
#include "redis_server.h"
#include "common/execution_gate.h"

namespace pkv {
bool redis_server::time(redis_coder& result) {
    const auto now = execution_time_us();
    auto seconds = now / 1000000;
    auto microseconds = now % 1000000;
    if (microseconds < 0) { microseconds += 1000000; --seconds; }
    auto& reply = result.create_object();
    reply.set_type(REDIS_OBJ_ARRAY);
    reply.create_child().set_string(std::to_string(seconds));
    reply.create_child().set_string(std::to_string(microseconds));
    return true;
}
} // namespace pkv
