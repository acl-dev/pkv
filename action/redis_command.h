//
// Created by shuxin zheng on 2023/8/1.
//

#pragma once
#include "coder/redis_coder.h"
#include "cluster/cluster_manager.h"

namespace pkv {

class redis_handler;

class redis_command {
public:
    redis_command(redis_handler& handler, const redis_object& obj);
    virtual ~redis_command() = default;

protected:
    redis_handler& handler_;
    const redis_object &obj_;

    bool check_cluster_mode(bool on) const;
    bool route_keys(const char* cmd, redis_coder& result);
    bool route_keys(const std::vector<std::string>& keys, redis_coder& result);
    bool route_slot(const std::string& key, size_t slot, redis_coder& result);
    bool redirect(const std::string& addr, size_t slot, redis_coder& result);
};

} // namespace pkv
