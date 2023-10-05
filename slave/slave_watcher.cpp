//
// Created by shuxin zheng on 2023/9/25.
//

#include "stdafx.h"
#include "slave_watcher.h"

namespace pkv {

static acl::atomic_long __count;

bool slave_watcher::on_set(const std::string &key, const std::string &value, bool ok) {
    if (clients_.empty()) {
        return true;
    }

    kv_message* message = new kv_message(key, value, message_oper_set);
    box_.push(message);
    return true;
}

bool slave_watcher::on_get(const std::string &key, const std::string &value, bool ok) {
    if (clients_.empty()) {
        return true;
    }

    kv_message* message = new kv_message(key, value, message_oper_get);
    ++__count;
    box_.push(message);
    return true;
}

bool slave_watcher::on_del(const std::string &key, bool ok) {
    if (clients_.empty()) {
        return true;
    }

    kv_message* message = new kv_message(key, "", message_oper_del);
    box_.push(message);
    return true;
}

void slave_watcher::stop() {
    eof_ = true;
}

void slave_watcher::run() {
    go[] {
        while (true) {
            ::sleep(1);
            printf(">>>count=%lld\r\n", __count.value());
        }
    };

    while (true) {
        kv_message* message = box_.pop(-1);
        if (message == nullptr) {
            if (eof_) {
                break;
            }
        } else {
            --__count;

            forward_message(message);
            message->unrefer();
        }
    }
}

void slave_watcher::forward_message(kv_message* message) {
    message->refer(clients_.size());
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
