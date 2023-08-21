//
// Created by shuxin ¡¡¡¡zheng on 2023/7/31.
//

#pragma once
#include "dao/json/json_string.h"
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

private:
    dao::string* dao_;

};

} // namespace pkv
