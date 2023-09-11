//
// Created by shuxin �heng on 2023/9/8.
//

#pragma once

namespace pkv {

class cluster_service : public acl::singleton<cluster_service> {
public:
    cluster_service() = default;
    ~cluster_service();

    bool bind(const char* addr, int max_slots);

    void add_slots(const std::vector<int>& slots);

private:
    acl::bitmap* slots_ = nullptr;
    acl::server_socket server_;
};

} // namespace pkv