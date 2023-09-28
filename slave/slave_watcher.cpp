//
// Created by shuxin zheng on 2023/9/25.
//

#include "stdafx.h"
#include "slave_watcher.h"

namespace pkv {

bool slave_watcher::on_set(const std::string &key, const std::string &value, bool ok) {
    shared_message message(new kv_message(key, value, message_oper_set));
    box_.push(message);
    return true;
}

bool slave_watcher::on_get(const std::string &key, const std::string &value, bool ok) {
    shared_message message(new kv_message(key, value, message_oper_get));
    box_.push(message);
    return true;
}

bool slave_watcher::on_del(const std::string &key, bool ok) {
    shared_message message(new kv_message(key, "", message_oper_del));
    box_.push(message);
    return true;
}

void slave_watcher::run() {
    while (true) {
        shared_message message;
        if (!box_.pop(message)) {
            logger_error("Pop message failed!");
            break;
        }

        if (message == nullptr) {
            logger("Pop null message and exit now!");
            break;
        }

        forward_message(message);
    }
}

void slave_watcher::forward_message(const shared_message& message) {
    for (auto& client : clients_) {
        client->push(message);
    }
}

void slave_watcher::add_client(const shared_client& client) {
    clients_.emplace_back(client);
}

bool slave_watcher::del_client(const shared_client& client) {
    for (auto it = clients_.begin(); it != clients_.end(); ++it) {
        if ((*it).get() == client.get()) {
            clients_.erase(it);
            return true;
        }
    }

    logger_error("Not found the client=%p", client.get());
    return false;
}

} // namespace pkv
