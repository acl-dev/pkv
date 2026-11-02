//
// Created by shuxin zheng on 2023/9/26.
//

#pragma once
#include "common/message_box.h"
#include "common/fiber_mailbox.h"
#include <mutex>
#include "common/replica_budget.h"

namespace pkv {

class slave_client : public std::enable_shared_from_this<slave_client> {
public:
    explicit slave_client(acl::shared_stream conn,
        size_t byte_limit = 64 * 1024 * 1024, size_t message_limit = 65536);
    ~slave_client();

    void run();

    void push(kv_message* message);
    void disconnect();
    size_t pending_bytes();
    size_t pending_messages();
    bool closed();

    acl::shared_stream& get_conn() {
        return conn_;
    }

private:
    acl::shared_stream conn_;
    fiber_mailbox<kv_message> box_;
    acl::fiber_mutex queue_lock_;
    replica_budget budget_;
    bool eof_ = false;
    bool negotiated_ = false;
    unsigned requested_version_ = 0;
    unsigned wire_version_ = 0;

    bool upgrade(unsigned version);

    void close_locked();
    void release(kv_message* message);
    void close_queue();
    void drain();
    bool flush(std::vector<kv_message*>& messages);
    bool legacy_pages(const kv_message& message);
};

using shared_client = std::shared_ptr<slave_client>;

} // namespace pkv
