#pragma once
#include "redis_command.h"
#include "common/pubsub_frame.h"
namespace pkv {
class redis_pubsub : public redis_command {
public:
    redis_pubsub(redis_handler& handler, const redis_object& object)
        : redis_command(handler, object) {}
    bool exec(const char* command, redis_coder& result);
private:
    bool change(pubsub_kind kind, bool subscribe, redis_coder& result);
    bool publish(bool shard, redis_coder& result);
    bool query(redis_coder& result);
    bool route(bool all, redis_coder& result);
};
} // namespace pkv
