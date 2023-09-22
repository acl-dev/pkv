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

    size_t max = 0;
    for (auto slot : slots) {
        slots_[slot] = node;
        if (slot > max) max = slot;
    }

    //show_null_slots();
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

    long long join_time = get_stamp();
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
        for (size_t slot = itv.first; slot <= itv.second; ++slot) {
            slots.push_back(slot);
        }
    }

    snode->add_slots(slots);

    for (auto slot : slots) {
        slots_[slot] = snode;
    }

    //show_null_slots();
    return true;
}

shared_node cluster_service::get_node(const std::string &addr) const {
    auto it = nodes_.find(addr);
    return it == nodes_.end() ? nullptr : it->second;
}

bool cluster_service::update_join_time(const std::string &addr) const {
    auto node = get_node(addr);
    if (node == nullptr) {
        logger_error("Not found the node with addr=%s", addr.c_str());
        return false;
    }

    long long now = get_stamp();
    node->set_join_time(now);
    return true;
}

shared_node cluster_service::get_node(const char *key, size_t& slot) {
    slot = hash_slot(key);
    auto node = slots_[slot];

#ifdef DEBUG_CLUSTER
    if (node == nullptr) {
        show_null_slots();
        assert(node);
    }
#endif
    return node;
}

size_t cluster_service::hash_slot(const char *key) const {
    unsigned hash = acl_hash_crc32(key, strlen(key));
    return hash % max_slots_;
}

long long cluster_service::get_stamp() {
    struct timeval now = { 0, 0 };
    gettimeofday(&now, nullptr);
    return now.tv_sec * 1000 + now.tv_usec / 1000;
}

void cluster_service::show_null_slots() const {
    for (size_t i = 0; i < slots_.size(); i++) {
        if (slots_[i] == nullptr) {
            printf(">>>%s: null slot=%zd\r\n", __func__, i);
        }
    }
}

} // namespace pkv
