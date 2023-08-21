//
// Created by shuxin ¡¡¡¡zheng on 2023/8/1.
//

#pragma once
#include "redis_command.h"

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

public:
    bool hset(redis_coder& result);
    bool hget(redis_coder& result);
    bool hdel(redis_coder& result);
    bool hmset(redis_coder& result);
    bool hmget(redis_coder& result);
    bool hgetall(redis_coder& result);
   
private:
    dao::hash* dao_;
};

} // namespace pkv
