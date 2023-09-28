//
// Created by shuxin ¡¡¡¡zheng on 2023/9/28.
//

#pragma once
#include "db/db.h"
#include "db/kv_message.h"

namespace pkv {

class cluster_client {
public:
    explicit cluster_client(std::string addr, shared_db db);
    ~cluster_client() = default;

    bool open();
    NORETURN void run();

private:
    std::string addr_;
    shared_db db_;
    acl::shared_stream conn_;

    void reopen();

    void handle_message(shared_message& message);
};

} // namespace pkv
