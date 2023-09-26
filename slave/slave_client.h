//
// Created by shuxin zheng on 2023/9/26.
//

#pragma once
#include "db/kv_message.h"

namespace pkv {

class slave_client {
public:
    explicit slave_client(acl::shared_stream conn);
    ~slave_client();

    void run();

    void push(shared_message message);

private:
    acl::shared_stream conn_;
    acl::fiber_tbox2<shared_message> box_;
};

using shared_client = std::shared_ptr<slave_client>;

} // namespace pkv
