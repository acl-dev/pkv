//
// Created by shuxin zheng on 2023/9/26.
//

#include "stdafx.h"
#include "slave/slave_client.h"
#include "slave/slave_watcher.h"
#include "cluster_service.h"
#include "rpc/password_handshake.h"

namespace pkv {

cluster_service::cluster_service(slave_watcher& watcher,
    const password_policy* passwords)
    : watcher_(watcher), passwords_(passwords) {}

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
        go_stack(var_cfg_slave_client_stack) [this, conn] {
            if (passwords_ && !accept_password(conn, *passwords_)) {
                conn->close(); return;
            }
            shared_client client(new slave_client(conn,
                var_cfg_replica_output_buffer_limit,
                var_cfg_replica_pending_messages));
            watcher_.add_client(client);
            client->run();
            auto& c = client->get_conn();
            logger("Disconnected from client %d, %s", c->sock_handle(),
                    c->get_peer(true));
            watcher_.del_client(client);
        };
    }
}

} // namespace pkv
