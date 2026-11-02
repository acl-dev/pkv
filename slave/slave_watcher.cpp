//
// Created by shuxin zheng on 2023/9/25.
//

#include "stdafx.h"
#include "common/message_box.h"
#include "slave_watcher.h"
#include "rpc/page_batch.h"
#include "common/execution_gate.h"

namespace pkv {

std::vector<shared_client> slave_watcher::invalidate_locked() {
    ++epoch_;
    std::vector<shared_client> clients;
    std::lock_guard<std::mutex> guard(clients_lock_);
    clients.swap(clients_);
    clients_count_.store(0);
    return clients;
}

bool slave_watcher::publish(const std::string& key, const std::string& value,
        message_oper_t operation, bool ok, unsigned database) {
    if (!ok || (clients_count_.load() == 0 && !collecting_.load())) {
        return true;
    }
    std::vector<shared_client> rejected;
    {
        std::lock_guard<std::mutex> guard(queue_lock_);
        if (eof_) {
            return true;
        }
        if (exec_active_) {
            capture_root_locked(database, key, value, operation);
            return true;
        }
        if (!budget_.reserve(key.size(), value.size())) {
            logger_error("Replica publish queue limit exceeded: bytes=%zu, "
                "messages=%zu", budget_.bytes(), budget_.messages());
            rejected = invalidate_locked();
        } else {
            try {
                std::unique_ptr<kv_message> message(new kv_message(key, value,
                    operation, database));
                std::unique_ptr<publication> item(new publication(message.get(),
                    epoch_));
                message.release();
                if (box_.push(item.get())) {
                    item.release();
                    return true;
                }
            } catch (const std::bad_alloc&) {
                logger_error("Replica publish allocation failed");
            }
            budget_.release(key.size(), value.size());
            rejected = invalidate_locked();
        }
    }
    // Never call a fiber-aware recipient while holding publisher metadata
    // locks.
    for (const auto& client : rejected) {
        client->disconnect();
    }
    return true;
}

bool slave_watcher::on_pages_database(unsigned database,
    const std::vector<db::batch_entry>& roots,
    const std::vector<page_change>& pages) {
    if (clients_count_.load() == 0 && !collecting_.load()) {
        return true;
    }
    page_batch_size size;
    const bool valid = page_batch::measure(database, roots, pages, size);
    std::vector<shared_client> rejected;
    {
        std::lock_guard<std::mutex> guard(queue_lock_);
        if (eof_) {
            return true;
        }
        if (exec_active_) {
            capture_pages_locked(database, roots, pages, valid ? &size :
                nullptr);
            return true;
        }
        if (!valid || !budget_.reserve_encoded(size.bytes, size.records)) {
            logger_error("Replica page publication exceeds queue limits");
            rejected = invalidate_locked();
        } else {
            try {
                auto batch = page_batch::capture(database, roots, pages);
                if (batch) {
                    std::unique_ptr<kv_message> message(new kv_message(batch));
                    std::unique_ptr<publication> item(new publication(
                        message.get(), epoch_));
                    message.release();
                    if (box_.push(item.get())) {
                        item.release();
                        return true;
                    }
                }
            } catch (const std::bad_alloc&) {
                logger_error("Replica page publication allocation failed");
            }
            budget_.release_encoded(size.bytes, size.records);
            rejected = invalidate_locked();
        }
    }
    for (const auto& client : rejected) {
        client->disconnect();
    }
    return true;
}

size_t slave_watcher::pending_bytes() {
    std::lock_guard<std::mutex> guard(queue_lock_);
    return budget_.bytes();
}

size_t slave_watcher::pending_messages() {
    std::lock_guard<std::mutex> guard(queue_lock_);
    return budget_.messages();
}

bool slave_watcher::on_set_database(unsigned database, const std::string& key,
    const std::string& value, bool ok) {
    return publish(key, value, message_oper_set, ok, database);
}

bool slave_watcher::on_del_database(unsigned database, const std::string& key,
    bool ok) {
    return publish(key, "", message_oper_del, ok, database);
}

bool slave_watcher::on_set(const std::string& key, const std::string& value,
    bool ok) {
    return publish(key, value, message_oper_set, ok);
}

bool slave_watcher::on_get(const std::string&, const std::string&, bool) {
    // Reads do not change replica state and must not amplify the write stream.
    return true;
}

bool slave_watcher::on_del(const std::string& key, bool ok) {
    return publish(key, "", message_oper_del, ok);
}

void slave_watcher::stop() {
    std::lock_guard<std::mutex> guard(queue_lock_);
    if (!eof_) {
        eof_ = true;
        box_.push(new publication(nullptr, epoch_));
    }
}

void slave_watcher::run() {
    while (auto pointer = box_.pop(-1)) {
        std::unique_ptr<publication> item(pointer);
        if (!item->message) {
            break;
        }
        forward_message(*item);
        std::lock_guard<std::mutex> guard(queue_lock_);
        budget_.release_encoded(item->message->wire_bytes(),
            item->message->wire_records());
    }
}

void slave_watcher::forward_message(const publication& item) {
    std::vector<shared_client> clients;
    {
        std::lock_guard<std::mutex> queue_guard(queue_lock_);
        if (item.epoch != epoch_) {
            return;
        }
        std::lock_guard<std::mutex> clients_guard(clients_lock_);
        clients = clients_;
    }
    for (const auto& client : clients) {
        item.message->refer(1);
        client->push(item.message);
    }
}

void slave_watcher::add_client(const shared_client& client) {
    execution_guard admission;
    bool stopped;
    {
        std::lock_guard<std::mutex> queue_guard(queue_lock_);
        stopped = eof_;
        if (!stopped) {
            std::lock_guard<std::mutex> clients_guard(clients_lock_);
            clients_.emplace_back(client);
            clients_count_.store(clients_.size());
        }
    }
    if (stopped) {
        client->disconnect();
    }
}

bool slave_watcher::del_client(const shared_client& client) {
    std::lock_guard<std::mutex> guard(clients_lock_);
    for (auto it = clients_.begin(); it != clients_.end(); ++it) {
        if ((*it).get() == client.get()) {
            clients_.erase(it);
            clients_count_.store(clients_.size());
            return true;
        }
    }

    // An overflow may already have detached this subscriber.
    return false;
}

} // namespace pkv
