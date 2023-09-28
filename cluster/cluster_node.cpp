//
// Created by shuxin zheng on 2023/9/15.
//

#include "stdafx.h"
#include "cluster_node.h"

#include <utility>

namespace pkv {

cluster_node::cluster_node(const char* addr)
: addr_(addr)
{
    slots_ = acl_dlink_create(10);
    struct timeval now = { 0, 0 };
    gettimeofday(&now, nullptr);
    id_  = std::to_string(now.tv_sec * 1000000) + std::to_string(now.tv_usec);
    id_ += "_";
    id_ += std::to_string(getpid());

    acl::string buf(addr);
    char* port = buf.rfind(":");
    if (port) {
        *port++ = 0;
        ip_ = buf.c_str();

        char* end;
        port_ = (int) std::strtol(port, &end, 10);
        if (*end != 0) {
            logger_error("Invalid port=%d, addr=%s", port_, addr);
        } else {
            rpc_port_ = port_ + SERVICE_RPC_PORT_ADD;
        }
    } else {
        logger_error("Invalid addr=%s", addr);
    }
}

cluster_node::~cluster_node() {
    acl_dlink_free(slots_);
}

cluster_node& cluster_node::set_myself(bool yes) {
    myself_ = yes;
    return *this;
}

cluster_node& cluster_node::set_id(const std::string& id) {
    id_ = id;
    return *this;
}

cluster_node & cluster_node::set_master_id(const std::string &id) {
    master_id_ = id;
    return *this;
}

cluster_node& cluster_node::set_type(const std::string& type) {
    acl::string buf(type.c_str());
    auto& tokens = buf.split2(",; \t");
    if (tokens.size() >= 2) {
        if (tokens[0].equal("myself", true)) {
            myself_ = true;
        }
        if (tokens[1].equal("master", true)) {
            type_ = node_type_master;
        }
    } else if (tokens[0].equal("master", true)) {
        type_ = node_type_master;
    }

    return *this;
}

cluster_node& cluster_node::set_type(int type) {
    if (type == (int) node_type_master) {
        type_ = node_type_master;
    } else if (type == (int) node_type_slave) {
        type_ = node_type_slave;
    } else {
        type_ = node_type_unknown;
    }
    return *this;
}

cluster_node& cluster_node::set_join_time(long long join_time) {
    join_time_ = join_time;
    return *this;
}

cluster_node& cluster_node::set_idx(int idx) {
    idx_ = idx;
    return *this;
}

cluster_node& cluster_node::set_connected(bool connected) {
    connected_ = connected;
    return *this;
}

void cluster_node::add_slots(const std::vector<size_t> &slots) {
    for (auto& slot : slots) {
        acl_dlink_insert(slots_, (long long) slot, (long long) slot);
    }
}

void cluster_node::get_slots(std::vector<std::pair<size_t, size_t>>& out) const {
    out.clear();

    ACL_ITER iter;
    acl_foreach(iter, slots_) {
        auto* item = (ACL_DITEM*) iter.data;
        out.emplace_back((size_t) item->begin, (size_t) item->end);
    }
}

void cluster_node::get_slots(std::vector<size_t> &out) const {
    out.clear();

    ACL_ITER iter;
    acl_foreach(iter, slots_) {
        auto* item = (ACL_DITEM*) iter.data;
        for (auto i = (size_t) item->begin; i <= (size_t) item->end; ++i) {
            out.push_back(i);
        }
    }
}

} // namespace pkv
