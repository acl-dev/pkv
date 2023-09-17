//
// Created by shuxin zheng on 2023/9/8.
//

#include "stdafx.h"
#include "cluster_service.h"

namespace pkv {

bool cluster_service::bind(const char *addr, size_t max_slots) {
    if (!server_.open(addr)) {
        logger_error("Bind %s error %s", addr, acl::last_serror());
        //return false;
    }
    logger("Bind %s ok", addr);

    if (max_slots > 0) {
        max_slots_ = max_slots;
    }

    slots_.clear();
    for (size_t i = 0; i < max_slots_; i++) {
        slots_.push_back(nullptr);
    }
    return true;
}

bool cluster_service::add_slots(const std::string &addr,
      const std::vector<size_t> &slots) {
    if (slots.size() > slots_.size()) {
        logger_error("Slots overflow, capacity=%zd, adding=%zd",
                slots_.size(), slots.size());
        return false;
    }

    shared_node node;
    auto it = nodes_.find(addr);
    if (it == nodes_.end()) {
        node = std::make_shared<cluster_node>(addr.c_str());
        nodes_[addr] = node;
    } else {
        node = it->second;
    }

    node->add_slots(slots);

    for (auto slot : slots) {
        slots_[slot] = node;
    }
    return true;
}

bool cluster_service::add_node(const acl::redis_node& node) {
    auto addr = node.get_addr();
    if (addr == nullptr || *addr == 0) {
        logger_error("Addr null");
        return false;
    }

    auto id = node.get_id();
    if (id == nullptr || *id == 0) {
        logger_error("ID empty");
        return false;
    }

    auto type = node.get_type();
    if (type == nullptr || *type == 0) {
        logger_error("TYPE empty");
        return false;
    }

    auto& slots_v = node.get_slots();

    struct timeval now;
    gettimeofday(&now, nullptr);
    long long join_time = now.tv_sec * 1000 + now.tv_usec / 1000;
    int idx = 0;
    bool connected = node.is_connected();

    shared_node snode;
    auto it = nodes_.find(addr);
    if (it == nodes_.end()) {
        snode = std::make_shared<cluster_node>(addr);
        nodes_[addr] = snode;
    } else {
        snode = it->second;
    }

    (*snode).set_id(id).set_type(type).set_join_time(join_time).set_idx(idx)
        .set_connected(connected);

    std::vector<size_t> slots;
    for (auto itv : slots_v) {
        for (size_t slot = itv.first; slot != itv.second; ++slot) {
            slots.push_back(slot);
        }
    }

    snode->add_slots(slots);

    for (auto slot : slots) {
        slots_[slot] = snode;
    }

    return true;
}

} // namespace pkv
