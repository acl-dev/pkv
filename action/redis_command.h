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
    bool redirect(const std::string& addr, size_t slot, redis_coder& result);
};

#define CHECK_AND_REDIRECT(cmd, obj, myaddr)  do {                             \
    if (obj.size() < 2) {                                                      \
        logger_error("cmd=%s, invalid params=%zd", cmd, obj.size());           \
        return false;                                                          \
    }                                                                          \
    auto key = obj[1];                                                         \
    if (key == nullptr || *key == 0) {                                         \
        logger_error("key null");                                              \
        return false;                                                          \
    }                                                                          \
    size_t slot;                                                               \
    auto node = cluster_manager::get_instance().get_node(key, slot);           \
    if (node == nullptr) {                                                     \
        return false;                                                          \
    }                                                                          \
    const auto& addr = node->get_addr();                                       \
    if (addr != myaddr) {                                                      \
        return this->redirect(addr, slot, result);                             \
    }                                                                          \
} while(0)

} // namespace pkv
