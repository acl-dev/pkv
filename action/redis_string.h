//
// Created by shuxin zheng on 2023/7/31.
//

#pragma once
#include "dao/string.h"
#include "redis_command.h"

namespace pkv {

namespace dao {
    class string;
}

class redis_coder;

class redis_string : public redis_command {
public:
    redis_string(redis_handler& handler, const redis_object& obj);
    ~redis_string() override;

    bool exec(const char* cmd, redis_coder& result);

public:
    bool set(redis_coder& result);
    bool get(redis_coder& result);
    bool digest(redis_coder& result);
    bool delex(redis_coder& result);
    bool lcs(redis_coder& result);
    bool mget(redis_coder& result);
    bool mset(redis_coder& result);
    bool msetex(redis_coder& result);
    bool increment(redis_coder& result);
    bool increment_float(redis_coder& result);
    bool append(redis_coder& result);
    bool setrange(redis_coder& result);
    bool length(redis_coder& result);
    bool getrange(redis_coder& result);
    bool getex(redis_coder& result);
    bool legacy_set(redis_coder& result);
    bool bitmap(redis_coder& result);
    bool bitcount(redis_coder& result);
    bool bitpos(redis_coder& result);
    bool bitop(redis_coder& result);
    bool bitfield(redis_coder& result);

private:
    dao::string* dao_;

};

} // namespace pkv
