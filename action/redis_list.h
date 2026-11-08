#pragma once
#include "redis_command.h"
#include "redis_dispatch.h"

namespace pkv {

class redis_list : public redis_command {
public:
    redis_list(redis_handler& handler, const redis_object& obj)
        : redis_command(handler, obj) {}
    bool exec(const char* cmd, redis_coder& result);
    static redis_handler_t bind(const char* name);
private:
    enum class operation : unsigned char {
        unknown, blpop, brpop, blmpop, blmove, brpoplpush,
        lmove, rpoplpush, lmpop, lpos,
        lpush, rpush, lpushx, rpushx, lpop, rpop,
        linsert, lrem, llen, lindex, lset, lrange, ltrim
    };
    static operation classify(const char* name);
    bool execute(operation op, const char* cmd, redis_coder& result);
    bool move(bool legacy, redis_coder& result);
    bool blocking(const char* command, redis_coder& result);
    bool positions(redis_coder& result);
    bool mpop(redis_coder& result);
};

} // namespace pkv
