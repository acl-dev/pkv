//
// Created by shuxin zheng on 2023/8/1.
//

#include "stdafx.h"
#include "coder/redis_coder.h"
#include "coder/redis_object.h"
#include "redis_command.h"

namespace pkv {

redis_command::redis_command(redis_handler& handler, const redis_object& obj)
: handler_(handler), obj_(obj)
{}

bool redis_command::redirect(const std::string &addr, size_t slot,
      redis_coder& result) {
    result.create_object().create_child().set_string("-MOVED", true)
        .create_child().set_string(std::to_string(slot), true)
        .create_child().set_string(addr);
    return true;
}

} // namespace pkv
