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

redis_cluster::~redis_cluster() = default;

struct cluster_handler {
    const char* cmd;
    bool (redis_cluster::*func) (redis_coder&);
};

static struct cluster_handler handlers[] = {
    { "NODES",      &redis_cluster::cluster_nodes           },
    { "ADDSLOTS",   &redis_cluster::cluster_addslots        },
    { "MEET",       &redis_cluster::cluster_meet            },
    { nullptr,      nullptr                                 },
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

bool redis_cluster::cluster_addslots(redis_coder& result) {
    if (obj_.size() < 3) {
        logger_error("Invalid CLUSTER ADDSLOTS params: %zd", obj_.size());
        return false;
    }

    std::vector<size_t> slots;
    for (size_t i = 2; i < obj_.size() && (int) i < var_cfg_redis_max_slots; i++) {
        int slot = atoi(obj_[i]);
        if (slot >= 0 && slot < var_cfg_redis_max_slots) {
            slots.push_back((size_t) slot);
        } else {
            logger_error("Invalid slot: %d", slot);
        }
    }

    cluster_service::get_instance().add_slots(var_cfg_service_addr, slots);
    result.create_object().set_status("OK");
    return true;
}

bool redis_cluster::cluster_nodes(redis_coder &result) {
    if (obj_.size() != 2) {
        logger_error("Invalid CLUSTER NODES params: %zd", obj_.size());
        return false;
    }

    build_nodes(result);
    return true;
}

void redis_cluster::build_nodes(redis_coder& result) {
    std::string buf;
    auto& nodes = cluster_service::get_instance().get_nodes();
    for (auto& node : nodes) {
        add_node(buf, node.first, *node.second);
    }
    result.create_object().set_string(buf);
}

void redis_cluster::add_node(std::string &buf, const std::string &addr,
       const cluster_node &node) {
    buf += node.get_id() + " ";
    buf += node.get_addr() + "@39002 ";
    buf += node.get_type() + " - 0 ";
    buf += std::to_string(node.get_join_time()) + " ";
    buf += std::to_string(node.get_idx()) + " ";
    buf += node.is_connected() ? "connected" : "disconnected";
    auto slots = node.get_slots();
    for (auto& slot : slots) {
        buf += " ";
        buf += std::to_string(slot.first) + "-" + std::to_string(slot.second);
    }
    buf += "\r\n";
}

bool redis_cluster::cluster_meet(redis_coder& result) {
    if (obj_.size() < 4) {
        logger_error("Invalid CLUSTER MEET params: %zd", obj_.size());
        return false;
    }

    const char* ip = obj_[2];
    if (ip == nullptr || *ip == 0) {
        logger_error("IP null");
        return false;
    }

    const char* port_s = obj_[3];
    if (port_s == nullptr || *port_s == 0) {
        logger_error("PORT null");
        return false;
    }
    int port = std::atoi(port_s);
    if (port <= 0 || port >= 65535) {
        logger_error("Invalid port=%d", port);
        return false;
    }

    std::string addr = ip;
    addr += ":";
    addr +=port_s;

    acl::redis_client conn(addr.c_str());
    acl::redis redis(&conn);
    auto* nodes = redis.cluster_nodes();
    if (nodes == nullptr) {
        logger_error("Request CLUSTER NODES error to addr=%s", addr.c_str());
        return false;
    }

    result.create_object().set_status("OK");

    add_nodes(*nodes);
    build_nodes(result);
    return true;
}

void redis_cluster::add_nodes(const std::map<acl::string, acl::redis_node*>& nodes) {
    for (auto& it : nodes) {
        cluster_service::get_instance().add_node(*it.second);
    }
}

} // namespace 
