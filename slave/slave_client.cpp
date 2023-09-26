//
// Created by shuxin zheng on 2023/9/26.
//

#include "stdafx.h"
#include "slave_client.h"

#include <utility>

namespace pkv {

slave_client::slave_client(acl::shared_stream conn) : conn_(std::move(conn)) {}
slave_client::~slave_client() = default;

void slave_client::run() {
    go[this] {
        while (true) {
            acl::string buf;
            if (conn_->gets(buf)) {
                printf("Gets: %s\r\n", buf.c_str());
            } else {
                printf("Gets EOF from %s\r\n", conn_->get_peer(true));
                break;
            }
        }

        // Notify the box_ to exit now.
        shared_message dummy;
        box_.push(dummy);
    };

    while (true) {
        shared_message message;
        if (!box_.pop(message)) {
            logger_error("Pop message failed");
            break;
        }

        if (message == nullptr) {
            logger("Pop null message and exit");
            break;
        }

        std::string buf;
        buf += "key: ";
        buf += message->get_key();
        buf += "; value: ";
        buf += message->get_value();
        buf += "; type: ";
        buf += message->get_type_s();
        buf += "\r\n";
        if (conn_->write(buf.c_str(), buf.size()) == -1) {
            logger_error("Send message to watcher error!");
            break;
        }
    }
}

void slave_client::push(shared_message message) {
    box_.push(std::move(message));
}

} // namespace pkv
