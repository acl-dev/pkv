//
// Created by shuxin zheng on 2023/8/1.
//

#pragma once
#include "dao/paged_hash.h"
#include "redis_command.h"
#include "redis_dispatch.h"

namespace pkv {

namespace dao {
    class hash;
} 

class redis_coder;

class redis_hash : public redis_command {
public:
    redis_hash(redis_handler& handler, const redis_object& obj);
    ~redis_hash() override;

    bool exec(const char* cmd, redis_coder& result);
    static redis_handler_t bind(const char* name);

public:
    bool hset(redis_coder& result);
    bool hget(redis_coder& result);
    bool hgetdel(redis_coder& result);
    bool hdel(redis_coder& result);
    bool hmset(redis_coder& result);
    bool hmget(redis_coder& result);
    bool hgetall(redis_coder& result);
    bool hsetnx(redis_coder& result);
    bool hincrby(redis_coder& result);
    bool hincrbyfloat(redis_coder& result);
    bool hlen(redis_coder& result);
    bool hstrlen(redis_coder& result);
    bool hexists(redis_coder& result);
    bool hkeys(redis_coder& result);
    bool hvals(redis_coder& result);
    bool hrandfield(redis_coder& result);
   
private:
    bool dispatch(const char* cmd, bool check_slot,
        bool (redis_hash::*operation)(redis_coder&), redis_coder& result);
    dao::paged_hash data_;
    dao::hash* dao_;
};

} // namespace pkv
