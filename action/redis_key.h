//
// Created by shuxin ¡¡¡¡zheng on 2023/7/31.
//

#pragma once
#include "redis_command.h"

namespace pkv {

namespace dao {
    class key;
}

class redis_handler;
class redis_object;
class redis_coder;
class db_cursor;

class redis_key : public redis_command {
public:
    redis_key(redis_handler& handler, const redis_object& obj);
    ~redis_key() override;

    bool exec(const char* cmd, redis_coder& result);

public:
    bool del(redis_coder& result);
    bool type(redis_coder& result);
    bool expire(redis_coder& result);
    bool ttl(redis_coder& result);
    bool scan(db_cursor& cursor, redis_coder& result);

private:
    dao::key* dao_;
};

} // namespace pkv
