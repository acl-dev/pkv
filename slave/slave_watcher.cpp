//
// Created by shuxin zheng on 2023/9/25.
//

#include "stdafx.h"
#include "common/message_box.h"
#include "slave_watcher.h"

namespace pkv {

static acl::atomic_long count;
static __thread message_box* current_box;

void slave_watcher::set_box(slave_watcher& watcher, message_box *box) {
    current_box = box;
    watcher.box_.push(box);
}

bool slave_watcher::on_set(const std::string &key, const std::string &value, bool ok) {
    if (clients_.empty()) {
        return true;
    }

    assert(current_box);

    auto message = new kv_message(key, value, message_oper_set);
    current_box->push(message);
    return true;
}

bool slave_watcher::on_get(const std::string &key, const std::string &value, bool ok) {
    if (clients_.empty()) {
        return true;
    }

    assert(current_box);

    auto message = new kv_message(key, value, message_oper_get);
    ++count;
    current_box->push(message);
    return true;
}

bool slave_watcher::on_del(const std::string &key, bool ok) {
    if (clients_.empty()) {
        return true;
    }

    assert(current_box);

    auto message = new kv_message(key, "", message_oper_del);
    current_box->push(message);
    return true;
}

void slave_watcher::stop() {
    eof_ = true;
    for (auto it : boxes_) {
        it->push(nullptr);
    }
}

void slave_watcher::run() {
    go[] {
        while (true) {
            ::sleep(1);
            printf(">>>count=%lld\r\n", count.value());
        }
    };

    while (true) {
        auto box = box_.pop();
        if (box) {
            boxes_.push_back(box);

            go [this, box] {
                while (true) {
                    auto message = box->pop(-1);
                    if (message == nullptr) {
                        if (eof_) {
                            break;
                        }
                    } else {
                        --count;
                        forward_message(message);
                        message->unrefer();
                    }
                }
            }; // End go.
        } // End box

        if (eof_) {
            break;
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
