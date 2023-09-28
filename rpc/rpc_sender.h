//
// Created by shuxin zheng on 2023/9/27.
//

#pragma once
#include "db/kv_message.h"

namespace pkv {

class rpc_sender {
public:
    rpc_sender() = default;
    ~rpc_sender() = default;

    static bool send(acl::shared_stream& conn, shared_message& message);
};

} // namespace pkv
