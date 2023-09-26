//
// Created by shuxin ¡¡¡¡zheng on 2023/9/26.
//

#pragma once
#include "cluster_manager.h"

namespace pkv {

using shared_server = std::shared_ptr<acl::server_socket>;

class cluster_service {
public:
    cluster_service() = default;
    ~cluster_service() = default;

    bool bind(const char* addr);

    NORETURN void run();

private:
    shared_server server_ = std::make_shared<acl::server_socket>();
};

} // namespace pkv
