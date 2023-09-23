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
    { "SLOTS",      &redis_cluster::cluster_slots           },
    { "NODES",      &redis_cluster::cluster_nodes           },
    { "ADDSLOTS",   &redis_cluster::cluster_addslots        },
    { "MEET",       &redis_cluster::cluster_meet            },

    // My extention of redis cluster commands.
    { "SYNCSLOTS",  &redis_cluster::cluster_syncslots       },

    { nullptr, nullptr,                               },
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

bool redis_cluster::cluster_slots(redis_coder& result) {
    if (obj_.size() != 2) {
        logger_error("Invalid CLUSTER SLOTS params: %zd", obj_.size());
        return false;
    }

    auto& obj = result.create_object();
    auto& nodes = cluster_service::get_instance().get_nodes();

    for (auto& item : nodes) {
        auto& node = item.second;
        auto& id = node->get_id();
        auto slots = node->get_slots();

        for (auto slot_pair : slots) {
            auto& child = obj.create_child();
            child.create_child().set_number(slot_pair.first, true)
                .create_child().set_number(slot_pair.second);
            auto& info = child.create_child();
            info.create_child().set_string(node->get_ip(), true)
                .create_child().set_number(node->get_port(), true)
                .create_child().set_string(id);
        }
    }

    return true;
}

bool redis_cluster::cluster_addslots(redis_coder& result) {
    if (obj_.size() < 3) {
        logger_error("Invalid CLUSTER ADDSLOTS params: %zd", obj_.size());
        return false;
    }

    std::vector<size_t> slots;
    for (size_t i = 2; i < obj_.size() && (int) i < var_cfg_redis_max_slots; i++) {
        int slot = std::atoi(obj_[i]);
        if (slot >= 0 && slot < var_cfg_redis_max_slots) {
            slots.push_back((size_t) slot);
        } else {
            logger_error("Invalid slot: %d", slot);
        }
    }

    cluster_service::get_instance().add_slots(var_cfg_service_addr, slots);
    //cluster_service::get_instance().show_null_slots();
    result.create_object().set_status("OK");
    return true;
}

bool redis_cluster::cluster_nodes(redis_coder &result) {
    if (obj_.size() != 2) {
        logger_error("Invalid CLUSTER NODES params: %zd", obj_.size());
        return false;
    }

    //cluster_service::get_instance().show_null_slots();
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

    // Update mine join time.
    if (!cluster_service::get_instance().update_join_time(var_cfg_service_addr)) {
        logger_error("Update the join time error for me=%s", var_cfg_service_addr);
        return false;
    }

    add_nodes(*nodes);

    // Notify the peer node to sync my slots info.
    if (!sync_slots(result.get_cache(), addr, var_cfg_service_addr)) {
        return false;
    }

    // Notify all the others to sync my slots info.
    auto& all_nodes = cluster_service::get_instance().get_nodes();
    for (auto it : all_nodes) {
        auto one_addr = it.second->get_addr();
        sync_slots(result.get_cache(), one_addr, var_cfg_service_addr);
    }

    // Save the current nodes info to the disk file.
    cluster_service::get_instance().save_nodes();

    result.create_object().set_status("OK");
    return true;
}

void redis_cluster::add_nodes(const std::map<acl::string, acl::redis_node*>& nodes) {
    for (auto& it : nodes) {
        cluster_service::get_instance().add_node(*it.second);
    }
}

bool redis_cluster::cluster_syncslots(redis_coder& result) {
    if (obj_.size() != 3) {
        logger_error("Invalid CLUSTER SYNCSLOTS params: %zd", obj_.size());
        return false;
    }

    const char* peer_addr = obj_[2];
    if (peer_addr == nullptr || *peer_addr == 0) {
        logger_error("peer address null");
        return false;
    }

    // Begin to get the peer node's SLOTS info.
    acl::redis_client conn(peer_addr);
    acl::redis redis(&conn);
    auto* nodes = redis.cluster_nodes();
    if (nodes == nullptr) {
        logger_error("Request CLUSTER NODES error from addr=%s", peer_addr);
        return false;
    }

    add_nodes(*nodes);

    // Save the current nodes info to the disk file.
    cluster_service::get_instance().save_nodes();

    result.create_object().set_status("OK");
    return true;
}

bool redis_cluster::sync_slots(redis_ocache& ocache, const std::string &addr,
      const char *myaddr) {
    acl::socket_stream conn;
    if (!conn.open(addr.c_str(), 60, 60)) {
        logger_error("Connect peer %s error %s", addr.c_str(), acl::last_serror());
        return false;
    }

    redis_coder coder(ocache);
    coder.create_object().create_child().set_string("CLUSTER", true)
        .create_child().set_string("SYNCSLOTS", true)
        .create_child().set_string(myaddr);

    std::string req;
    if (!coder.to_string(req)) {
        logger_error("Create request command error!");
        return false;
    }

    // Send CLUSTER SYNCSLOTS to peer node.
    if (conn.write(req.c_str(), req.size()) != (int) req.size()) {
        logger_error("Send request(%s) to %s error", req.c_str(), addr.c_str());
        return false;
    }

    coder.clear();

    // Wait the peer node's response.

    char buf[4096];
    while (true) {
        int ret = conn.read(buf, sizeof(buf) - 1, false);
        if (ret < 0) {
            logger_error("Read response error from %s", addr.c_str());
            return false;
        }

        buf[ret] = 0;

        size_t len = ret;
        (void) coder.update(buf, len);
        auto& objs = coder.get_objects();
        if (objs.empty()) {
            continue;
        }

        if (objs.size() != 1) {
            logger_error("Invalid reply objs' size=%zd", objs.size());
            return false;
        }

        auto obj = objs[0];
        if (obj->failed()) {
            logger_error("Invalid data: %s", buf);
            return false;
        }
        if (obj->finish()) {
            auto type = obj->get_type();
            if (type != REDIS_OBJ_STATUS) {
                logger_error("Invalid type=%d, not status type", (int) type);
                return false;
            }

            auto& data = obj->get_buf();
            if (data.empty()) {
                logger_error("No data got for status");
                return false;
            }

            if (strcasecmp(data.c_str(), "OK") != 0) {
                logger_error("Invalid status=%s", data.c_str());
                return false;
            }
            return true;
        }
    }
}

} // namespace
