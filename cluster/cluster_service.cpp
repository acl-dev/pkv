//
// Created by shuxin ¡¡¡¡zheng on 2023/9/26.
//

#include "stdafx.h"
#include "slave/slave_client.h"
#include "cluster_service.h"

namespace pkv {

bool cluster_service::bind(const char *addr) {
    if (!server_->open(addr)) {
        logger_error("Open %s error %s", addr, acl::last_serror());
        return false;
    }

    logger("Bind %s ok", addr);
    return true;
}

void cluster_service::run() {
    while (true) {
        auto conn = server_->shared_accept();
        if (conn == nullptr) {
            logger_error("Accept error %s", acl::last_serror());
            ::sleep(1);
            continue;
        }

        // Start one client fiber to handle the connection from other node.
        go[conn] {
            std::unique_ptr<slave_client> client(new slave_client(conn));
            client->run();
        };
    }
}

} // namespace pkv
