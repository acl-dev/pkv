//
// Created by shuxin zheng on 2023/9/26.
//

#include "stdafx.h"
#include "rpc/rpc_sender.h"
#include "rpc/rpc_message.h"
#include "rpc/exec_batch.h"
#include "slave_client.h"

namespace pkv {

slave_client::slave_client(acl::shared_stream conn,
    size_t byte_limit, size_t message_limit)
: conn_(std::move(conn))
, budget_(byte_limit, message_limit)
{
    conn_->set_rw_timeout(-1);
}
slave_client::~slave_client() {
    close_queue();
    drain();
}

void slave_client::close_locked() {
    if (!eof_) {
        eof_ = true;
        box_.close();
        // shutdown is safe across threads and wakes a blocked socket write.
        // Actual close is serialized below to avoid descriptor reuse races.
#ifdef _WIN32
        ::shutdown(conn_->sock_handle(), SD_BOTH);
#else
        ::shutdown(conn_->sock_handle(), SHUT_RDWR);
#endif
    }
}

void slave_client::close_queue() {
    std::lock_guard<acl::fiber_mutex> guard(queue_lock_);
    close_locked();
}

void slave_client::disconnect() {
    close_queue();
}

size_t slave_client::pending_bytes() {
    std::lock_guard<acl::fiber_mutex> guard(queue_lock_);
    return budget_.bytes();
}

size_t slave_client::pending_messages() {
    std::lock_guard<acl::fiber_mutex> guard(queue_lock_);
    return budget_.messages();
}

bool slave_client::closed() {
    std::lock_guard<acl::fiber_mutex> guard(queue_lock_);
    return eof_;
}

void slave_client::release(kv_message* message) {
    std::lock_guard<acl::fiber_mutex> guard(queue_lock_);
    budget_.release_encoded(message->wire_bytes(), message->wire_records());
    message->unrefer();
}

void slave_client::drain() {
    while (auto message = box_.pop(0)) {
        release(message);
    }
}

void slave_client::run() {
    auto self = shared_from_this();
    fiber_mailbox<bool> reader_done;
    bool finished = true;
    // Start one fiber to handle message from peer client.
    go[this, self, &reader_done, &finished] {
        while (true) {
            acl::string line;
            if (!conn_->gets(line, true, 64)) {
                break;
            }
            const unsigned version = line == "PKV-REPL 4" ? 4 :
                line == "PKV-REPL 3" ? 3 :
                line == "PKV-REPL 2" ? 2 :
                line == "PKV-REPL 1" ? 1 : 0;
            if (!version || !upgrade(version)) {
                break;
            }
        }

        close_queue();
        reader_done.push(&finished);
    };

    std::vector<kv_message*> messages;
    int timeout = -1;

    // Wait and handle the messsge from box been putting by slave_watcher.
    while (true) {
        bool mailbox_closed = false;
        auto message = box_.pop(timeout, &mailbox_closed);
        if (message == nullptr) {
            bool closed;
            {
                std::lock_guard<acl::fiber_mutex> guard(queue_lock_);
                if (mailbox_closed) close_locked();
                closed = eof_;
            }
            if (closed) {
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

        if (closed()) {
            release(message);
            break;
        }

        if (message->get_oper() == message_oper_ready) {
            if (!flush(messages)) {
                release(message);
                break;
            }
            const auto version = message->get_protocol();
            const bool sent = rpc_sender::send_versioned(conn_,
                std::vector<kv_message*>{message}, version);
            release(message);
            if (!sent) {
                break;
            }
            wire_version_ = version;
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

    close_queue();
    // shutdown wakes gets(); wait until it releases ACL's socket event before
    // close destroys that event. The queue lock alone cannot protect gets().
    reader_done.pop();
    {
        std::lock_guard<acl::fiber_mutex> guard(queue_lock_);
        conn_->close();
    }
    for (auto message : messages) {
        release(message);
    }
    drain();
}

bool slave_client::flush(std::vector<kv_message*>& messages) {
    bool ret = wire_version_ ? rpc_sender::send_versioned(conn_, messages,
        wire_version_) : rpc_sender::send(conn_, messages);
    for (auto it : messages) {
        release(it);
    }
    messages.clear();
    return ret;
}

bool slave_client::upgrade(unsigned version) {
    std::lock_guard<acl::fiber_mutex> guard(queue_lock_);
    if (eof_ || (negotiated_ && requested_version_ != version)) {
        return false;
    }
    if (!negotiated_) {
        if (!budget_.reserve(0, 0)) {
            logger_error("Replica output queue limit exceeded during upgrade");
            close_locked();
            return false;
        }
        try {
            std::unique_ptr<kv_message> ready(new kv_message("", "",
                message_oper_ready, 0, version));
            if (!box_.push(ready.get())) throw std::bad_alloc();
            ready.release();
        } catch (const std::bad_alloc&) {
            budget_.release_encoded(20, 1);
            close_locked(); return false;
        }
        negotiated_ = true;
        requested_version_ = version;
    }
    return true;
}

void slave_client::push(kv_message* message) {
    std::lock_guard<acl::fiber_mutex> guard(queue_lock_);
    if (eof_ || (message->get_database() != 0 && !negotiated_)) {
        message->unrefer();
    } else if (message->get_oper() == message_oper_exec &&
        requested_version_ < message->get_protocol()) {
        // Legacy subscribers see DB 0 only. An unrelated whole group can be
        // filtered intact; a group containing DB 0 cannot be downgraded.
        const auto group = message->get_exec();
        if (!negotiated_ && group && !group->touches(0)) {
            message->unrefer(); return;
        }
        message->unrefer();
        close_locked();
    } else if (message->get_oper() == message_oper_pages &&
        requested_version_ < 2) {
        const bool accepted = legacy_pages(*message);
        message->unrefer();
        if (!accepted) {
            logger_error("Replica peer cannot receive page transaction");
            close_locked();
        }
    } else if (!budget_.reserve_encoded(message->wire_bytes(),
        message->wire_records())) {
        logger_error("Replica output queue limit exceeded: bytes=%zu, "
            "messages=%zu", budget_.bytes(), budget_.messages());
        message->unrefer();
        close_locked();
    } else {
        if (!box_.push(message)) {
            budget_.release_encoded(message->wire_bytes(),
                message->wire_records());
            message->unrefer(); close_locked();
        }
    }
}

} // namespace pkv
