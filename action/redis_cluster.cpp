//
// Created by shuxin zheng on 2023/9/8.
//

#include "stdafx.h"
#include "coder/redis_coder.h"
#include "coder/redis_object.h"
#include "cluster/cluster_manager.h"

#include "redis_cluster.h"

namespace pkv {

#define EQ  !strcasecmp

redis_cluster::redis_cluster(redis_handler& handler, const redis_object& obj)
: redis_command(handler, obj)
, manager_(cluster_manager::get_instance())
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
    { "REPLICATE",  &redis_cluster::cluster_replicate       },

    // My extention of redis cluster commands.
    { "SYNCSLOTS",  &redis_cluster::cluster_syncslots       },
    { "ADDSLAVE",   &redis_cluster::cluster_addslave        },

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
    auto& nodes = manager_.get_nodes();

    for (auto& item : nodes) {
        auto& node = item.second;
        // Skip the no-master nodes.
        if (!node->is_master()) {
            continue;
        }

        auto& id = node->get_id();
        std::vector<std::pair<size_t, size_t>> slots;
        node->get_slots(slots);

        for (auto slot_pair : slots) {
            auto& child = obj.create_child();
            child.create_child().set_number((long long) slot_pair.first, true)
                .create_child().set_number((long long) slot_pair.second);
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
    for (size_t i = 2; i < obj_.size(); i++) {
        char* end;
        int slot = (int) std::strtol(obj_[i], &end, 10);
        if (*end == 0 && slot >= 0 && slot < var_cfg_redis_max_slots) {
            slots.push_back((size_t) slot);
        } else {
            logger_error("Invalid slot: %d, left=%s", slot, end);
        }
    }

    // Add the current node slots.
    auto node = manager_.add_slots(var_cfg_service_addr, slots, true);
    if (node == nullptr) {
        logger_error("Add slots error");
        return false;
    }

    (*node).set_join_time(cluster_manager::get_stamp())
        .set_connected(true)
        .set_type("master")
        .set_myself(true);

    // Bind the current node object.
    manager_.set_me(node);

    // Set the current node be in master mode.
    manager_.set_master_mode(true);

    //manager_.show_null_slots();
    result.create_object().set_status("OK");
    return true;
}

bool redis_cluster::cluster_replicate(redis_coder& result) {
    if (obj_.size() < 3) {
        logger_error("Invalid CLUSTER REPLICATE params: %zd", obj_.size());
        return false;
    }
    result.create_object().set_status("OK");
    return true;
}

bool redis_cluster::cluster_nodes(redis_coder &result) {
    if (obj_.size() != 2) {
        logger_error("Invalid CLUSTER NODES params: %zd", obj_.size());
        return false;
    }

    //cluster_manager::get_instance().show_null_slots();
    build_nodes(result);
    return true;
}

void redis_cluster::build_nodes(redis_coder& result) {
    std::string buf;
    auto& nodes = manager_.get_nodes();
    for (auto& node : nodes) {
        add_node(buf, *node.second);
    }
    result.create_object().set_string(buf);
    //printf(">>>%s\r\n", buf.c_str());
}

void redis_cluster::add_node(std::string &buf, const cluster_node &node) {
    buf += node.get_id() + " ";
    buf += node.get_addr() + "@" + std::to_string(node.get_rpc_port()) + " ";
    if (node.is_myself()) {
        buf += "myself,";
    }

    buf += node.get_type_s() + " ";
    buf += node.get_master_id() + " ";
    buf += "0 ";
    buf += std::to_string(node.get_join_time()) + " ";
    buf += std::to_string(node.get_idx()) + " ";
    buf += node.is_connected() ? "connected" : "disconnected";

    std::vector<std::pair<size_t, size_t>> slots;
    node.get_slots(slots);

    for (auto& slot : slots) {
        buf += " ";
        buf += std::to_string(slot.first) + "-" + std::to_string(slot.second);
    }
    buf += "\r\n";
}

void redis_cluster::add_nodes(const std::map<acl::string, acl::redis_node*>& nodes) {
    for (auto& it : nodes) {
        manager_.add_node(*it.second, it.second->is_master());

        auto slaves = it.second->get_slaves();
        if (!slaves) {
            continue;
        }

        // Try to add the slaves of the master when the node is one master node.
        for (auto slave : *slaves) {
            auto node = manager_.add_node(*slave, false);
            if (node && node->get_addr() == var_cfg_service_addr) {
                node->set_myself(true);
                manager_.set_me(node);
            }
        }
    }
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

    char* end;
    int port = (int) std::strtol(port_s, &end, 10);
    if (*end != 0 || port <= 0 || port >= 65535) {
        logger_error("Invalid port=%d, %s, end=%s", port, port_s, end);
        return false;
    }

    std::string addr = ip;
    addr += ":";
    addr += port_s;

    acl::redis_client conn(addr.c_str());
    acl::redis redis(&conn);
    auto* nodes = redis.cluster_nodes();
    if (nodes == nullptr) {
        logger_error("Request CLUSTER NODES error to addr=%s", addr.c_str());
        return false;
    }

    add_nodes(*nodes);

    auto me = manager_.get_me();
    // Update my join time.
    if (me != nullptr) {
        me->set_join_time(cluster_manager::get_stamp());
    }

    if (manager_.is_master_mode()) {
        notify_nodes(result.get_cache());
    }
    // Should be in slave mode.
    else {
        auto ss = conn.get_stream();
        if (ss == nullptr) {
            logger_error("Disonnect from master=%s", addr.c_str());
            return false;
        }

        acl::string rpc_addr;
        int rpc_port = port + SERVICE_RPC_PORT_ADD;  // XXX
        rpc_addr.format("%s:%d", ip, rpc_port);
        if (!link_master(result.get_cache(), *ss,
                rpc_addr, var_cfg_service_addr)) {
            logger_error("Bind master failed, master=%s, rpc=%s",
                         addr.c_str(), rpc_addr.c_str());
            return false;
        }
    }

    // Save the current nodes info to the disk file.
    if (!manager_.save_nodes()) {
    	logger_error("Save cluster nodes info failed");
    }

    result.create_object().set_status("OK");
    return true;
}

void redis_cluster::notify_nodes(redis_ocache& ocache) {
    // Notify all the others to sync my slots info.
    auto& all_nodes = manager_.get_nodes();
    for (const auto& it : all_nodes) {
        if (it.second->is_myself()) {
            continue;
        }

        auto one_addr = it.second->get_addr();
        if (!sync_slots(ocache, one_addr, var_cfg_service_addr)) {
            logger_error("Notify %s sync slots error", one_addr.c_str());
        } else {
            it.second->set_connected(true);
            logger("Notify sync slots ok, addr=%s, type=%s",
                   it.second->get_addr().c_str(),
                   it.second->get_type_s().c_str());
        }
    }
}

bool redis_cluster::link_master(redis_ocache& ocache, acl::socket_stream& conn,
      const std::string& rpc_addr, const std::string& myaddr) {

    redis_coder coder(ocache);
    coder.create_object().create_child().set_string("CLUSTER", true)
        .create_child().set_string("ADDSLAVE", true)
        .create_child().set_string(myaddr);

    std::string req;
    if (!coder.to_string(req)) {
        logger_error("Create request command error");
        return false;
    }

    if (conn.write(req.c_str(), req.size()) != (int) req.size()) {
        logger_error("Send request(%s) to %s error", req.c_str(), conn.get_peer());
        return false;
    }

    if (!read_status(conn, coder)) {
        logger_error("Bind master(%s) failed", conn.get_peer(true));
        return false;
    }

    if (!manager_.sync_open(rpc_addr)) {
        logger_error("Connect master(%s) error", rpc_addr.c_str());
        return false;
    }
    return true;
}

bool redis_cluster::cluster_addslave(redis_coder &result) {
    if (obj_.size() < 3) {
        logger_error("Invalid CLUSTER ADDSLAVE params: %zd", obj_.size());
        return false;
    }

    const char* peer_addr = obj_[2];
    if (peer_addr == nullptr || *peer_addr == 0) {
        logger_error("peer address null");
        return false;
    }

    if (!manager_.is_master_mode()) {
        logger_error("I'm not a master node, addr=%s", var_cfg_service_addr);
        return false;
    }

    auto me = manager_.get_me();
    if (me == nullptr) {
        logger_error("I havn't connected yet, addr=%s", var_cfg_service_addr);
        return false;
    }

    std::vector<size_t> slots;
    me->get_slots(slots);

    auto node = manager_.get_node(peer_addr);
    if (node == nullptr) {
        node = manager_.add_slots(peer_addr, slots, false);
    }

    if (node == nullptr) {
        logger_error("Add slave node error");
        return false;
    }

    node->set_connected(true)
        .set_join_time(cluster_manager::get_stamp())
        .set_master_id(me->get_id());

    notify_nodes(result.get_cache());

    result.create_object().set_status("OK");
    return true;
}

bool redis_cluster::cluster_syncslots(redis_coder& result) {
    if (obj_.size() < 3) {
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
    if (!manager_.save_nodes()) {
        logger_error("Save cluster nodes info failed");
    }

    result.create_object().set_status("OK");
    return true;
}

bool redis_cluster::sync_slots(redis_ocache& ocache, const std::string &addr,
      const char *myaddr) {
    acl::socket_stream conn;
    if (!conn.open(addr.c_str(), 60, 60)) {
        logger_error("Connect peer %s error %s", addr.c_str(),
                     acl::last_serror());
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

    return read_status(conn, coder);
}

bool redis_cluster::read_status(acl::socket_stream& conn, redis_coder& coder) {
    coder.clear();

    // Wait the peer node's response.

    char buf[4096];
    while (true) {
        int ret = conn.read(buf, sizeof(buf) - 1, false);
        if (ret < 0) {
            logger_error("Read response error from %s", conn.get_peer(true));
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
