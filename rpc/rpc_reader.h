//
// Created by shuxin zheng on 2023/9/27.
//

#pragma once
#include "common/kv_message.h"

namespace pkv {

class rpc_reader {
public:
    rpc_reader() = default;
    ~rpc_reader() = default;

    static bool read(acl::shared_stream& conn, shared_message& message);
    static bool read(acl::shared_stream& conn, size_t len, std::string& out);
};

} // namespace pkv
