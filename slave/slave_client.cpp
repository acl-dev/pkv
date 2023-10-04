//
// Created by shuxin zheng on 2023/9/26.
//

#include "stdafx.h"
#include "rpc/rpc_sender.h"
#include "slave_client.h"

#include <utility>

namespace pkv {

slave_client::slave_client(acl::shared_stream conn) : conn_(std::move(conn)) {}
slave_client::~slave_client() = default;

void slave_client::run() {
    conn_->set_rw_timeout(-1);

    auto self = shared_from_this();
    // Start one fiber to handle message from peer client.
    go[this, self] {
        while (true) {
            acl::string buf;
            if (conn_->gets(buf)) {
                printf("Gets: %s\r\n", buf.c_str());
            } else {
                printf("Gets EOF from %s\r\n", conn_->get_peer(true));
                break;
            }
        }

        eof_ = true;

        // Notify the box_ to exit now.
        box_.push(nullptr);
    };

    std::vector<kv_message*> messages;
    int timeout = -1;
    bool success;

    // Wait and handle the messsge from box been putting by slave_watcher.
    while (true) {
        kv_message* message = box_.pop(timeout, &success);
        if (message == nullptr) {
            if (!success) {
                logger("Pop null message and exit");
                break;
            }
            if (eof_) {
                logger("EOF and exit");
                break;
            }

            if (!flush(messages)) {
                logger_error("flush to client error %s", acl::last_serror());
                break;
            }

            messages.clear();
            timeout = -1;
            continue;
        }

        messages.push_back(message);
        if (messages.size() >= (size_t) var_cfg_slave_messages_flush) {
            if (!flush(messages)) {
                logger_error("flush to client error %s", acl::last_serror());
                break;
            }
            messages.clear();
            timeout = -1;
        } else {
            timeout = 0;
        }
    }

    for (auto it : messages) {
        it->unrefer();
    }
}

bool slave_client::flush(std::vector<kv_message*>& messages) {
    bool ret = rpc_sender::send(conn_, messages);
    for (auto it : messages) {
        it->unrefer();
    }
    return ret;
}

void slave_client::push(kv_message* message) {
    box_.push(message);
}

} // namespace pkv
