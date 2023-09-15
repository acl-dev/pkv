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
    struct timeval now;
    gettimeofday(&now, NULL);
    id_ = std::to_string(now.tv_sec * 1000000) + std::to_string(now.tv_usec)
        + std::to_string(getpid());
}

cluster_node::~cluster_node() {
    acl_dlink_free(slots_);
}

void cluster_node::add_slots(const std::vector<int> &slots) {
    for (auto& slot : slots) {
        acl_dlink_insert(slots_, slot, slot);
    }
}

std::vector<std::pair<int, int>> cluster_node::get_slots() const {
    std::vector<std::pair<int, int>> slots;
    ACL_ITER iter;
    acl_foreach(iter, slots_) {
        auto* item = (ACL_DITEM*) iter.data;
        slots.emplace_back(item->begin, item->end);
    }
    return slots;
}

} // namespace pkv