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
        port_ = std::atoi(port);
    } else {
        logger_error("Invalid addr=%s", addr);
    }
}

cluster_node::~cluster_node() {
    acl_dlink_free(slots_);
}

cluster_node& cluster_node::set_id(const std::string& id) {
    id_ = id;
    return *this;
}

cluster_node& cluster_node::set_type(const std::string& type) {
    type_ = type;
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

std::vector<std::pair<size_t, size_t>> cluster_node::get_slots() const {
    std::vector<std::pair<size_t, size_t>> slots;
    ACL_ITER iter;
    acl_foreach(iter, slots_) {
        auto* item = (ACL_DITEM*) iter.data;
        slots.emplace_back((size_t) item->begin, (size_t) item->end);
    }
    return slots;
}

} // namespace pkv
