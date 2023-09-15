//
// Created by shuxin zheng on 2023/9/8.
//

#include "stdafx.h"
#include "cluster_service.h"

namespace pkv {

bool cluster_service::bind(const char *addr, size_t max_slots) {
    if (!server_.open(addr)) {
        logger_error("Bind %s error %s", addr, acl::last_serror());
        return false;
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

void cluster_service::add_slots(const std::string &addr,
      const std::vector<int> &slots) {
    shared_node node;
    auto it = nodes_.find(addr);
    if (it == nodes_.end()) {
        node = std::make_shared<cluster_node>(addr.c_str());
        nodes_[addr] = node;
    } else {
        node = it->second;
    }

    node->add_slots(slots);

    for (auto& slot : slots) {
        slots_[slot] = node;
    }
}

} // namespace pkv