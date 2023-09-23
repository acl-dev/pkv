//
// Created by shuxin zheng on 2023/8/1.
//

<<<<<<< HEAD
=======
#include "stdafx.h"
#include "coder/redis_coder.h"
#include "coder/redis_object.h"
>>>>>>> master
#include "redis_command.h"

namespace pkv {

redis_command::redis_command(redis_handler& handler, const redis_object& obj)
: handler_(handler), obj_(obj)
{}

<<<<<<< HEAD
=======
bool redis_command::check_cluster_mode(bool on) const {
    return var_cfg_cluster_mode != 0 && on;
}

bool redis_command::redirect(const std::string &addr, size_t slot,
      redis_coder& result) {
    std::string buf("MOVED");
    buf += " ";
    buf += std::to_string(slot);
    buf += " ";
    buf += addr;
    result.create_object().set_error(buf);
    return true;
}

>>>>>>> master
} // namespace pkv
