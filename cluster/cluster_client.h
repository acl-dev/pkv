//
// Created by shuxin ¡¡¡¡zheng on 2023/9/28.
//

#pragma once
#include "db/db.h"
#include "common/kv_message.h"
#include "rpc/page_batch.h"
#include "rpc/exec_batch.h"
#include "rpc/ordered_receiver.h"

namespace pkv {

class database_pool;
struct rpc_packet;

class cluster_client {
public:
    explicit cluster_client(std::string addr, shared_db db,
        std::shared_ptr<database_pool> databases = nullptr,
        std::string password = {});
    ~cluster_client() = default;

    bool open();
    NORETURN void run();
    bool apply(const shared_message& message);
    bool apply_packet(rpc_packet packet);

private:
    std::string addr_;
    std::string password_;
    shared_db db_;
    std::shared_ptr<database_pool> databases_;
    acl::shared_stream conn_;
    bool negotiated_ = false;
    unsigned wire_version_ = 0;
    unsigned requested_version_ = 5;
    unsigned next_version_ = 5;
    page_receiver pages_;
    exec_receiver executions_;
    ordered_receiver ordered_;

    shared_db select(unsigned database);

    void reopen();


};

} // namespace pkv
