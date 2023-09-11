//
// Created by shuxin zheng on 2023/9/8.
//

#include "stdafx.h"
#include "coder/redis_coder.h"
#include "coder/redis_object.h"
#include "cluster/cluster_service.h"

#include "redis_cluster.h"

namespace pkv {

#define EQ  !strcasecmp

redis_cluster::redis_cluster(redis_handler& handler, const redis_object& obj)
: redis_command(handler, obj)
{
}

redis_cluster::~redis_cluster() {}

struct cluster_handler {
    const char* cmd;
    bool (redis_cluster::*func) (redis_coder&);
};

static struct cluster_handler handlers[] = {
    { "ADDSLOTS",       &redis_cluster::addslots       },
    { nullptr,      nullptr                 },
};

bool redis_cluster::exec(const char*, redis_coder& result) {
    if (obj_.size() < 2) {
        logger_error("Invalid CLUSTER parameters: %zd", obj_.size());
        return false;
    }

    const char* cmd = obj_[1];
    if (cmd == nullptr || *cmd == 0) {
        logger_error("Invalid CLUSTER's sub-command");
        return false;
    }

    for (int i = 0; handlers[i].cmd != nullptr; i++) {
        if (EQ(cmd, handlers[i].cmd)) {
            return (this->*(handlers[i].func))(result);
        }
    }

    logger_error("Not support, cmd=%s", cmd);
    return false;
}

bool redis_cluster::addslots(redis_coder& result) {
    if (obj_.size() < 3) {
        logger_error("Invalid CLUSTER ADDSLOTS params: %zd", obj_.size());
        return false;
    }

    std::vector<int> slots;
    for (size_t i = 0; i < obj_.size(); i++) {
        int slot = atoi(obj_[i]);
        if (slot >= 0 && slot < var_cfg_redis_max_slots) {
            slots.push_back(slot);
        } else {
            logger_error("Invalid slot: %d", slot);
        }
    }

    cluster_service::get_instance().add_slots(slots);
    result.create_object().set_status("OK");
    return true;
}

} // namespace 

