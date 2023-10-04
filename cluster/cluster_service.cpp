//
// Created by shuxin zheng on 2023/9/26.
//

#include "stdafx.h"
#include "slave/slave_client.h"
#include "slave/slave_watcher.h"
#include "cluster_service.h"

namespace pkv {

cluster_service::cluster_service(slave_watcher &watcher) : watcher_(watcher) {}

bool cluster_service::bind(const char *addr) {
    if (!server_->open(addr)) {
        logger_error("Open %s error %s", addr, acl::last_serror());
        return false;
    }

    logger("Bind %s ok", addr);
    return true;
}

void cluster_service::run() {
    // Loop waiting connection from client and create fiber for each connection
    // to forward messages to them.
    while (true) {
        auto conn = server_->shared_accept();
        if (conn == nullptr) {
            logger_error("Accept error %s", acl::last_serror());
            ::sleep(1);
            continue;
        }

        // Start one client fiber to handle the connection from other node.
        go[this, conn] {
            shared_client client(new slave_client(conn));
            watcher_.add_client(client);
            client->run();
            watcher_.del_client(client);
        };
    }
}

} // namespace pkv
