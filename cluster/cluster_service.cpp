//
// Created by shuxin zheng on 2023/9/8.
//

#include "stdafx.h"
#include "cluster_service.h"

namespace pkv {

cluster_service::~cluster_service() {
    delete slots_;
}

bool cluster_service::bind(const char *addr, int max_slots) {
    if (!server_.open(addr)) {
        logger_error("Bind %s error %s", addr, acl::last_serror());
        return false;
    }
    logger("Bind %s ok", addr);

    assert(max_slots > 0);

    slots_ = new acl::bitmap(max_slots);
    return true;
}

void cluster_service::add_slots(const std::vector<int> &slots) {
    for (auto& slot : slots) {
        slots_->bit_set((size_t) slot);
    }
}

} // namespace pkv