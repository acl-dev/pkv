//
// Created by shuxin zheng on 2023/9/27.
//

#pragma once
#include "common/kv_message.h"
#include "page_batch.h"

namespace pkv {

struct rpc_packet {
    shared_message message;
    page_frame frame;
    bool page = false;
};

class rpc_reader {
public:
    rpc_reader() = default;
    ~rpc_reader() = default;

    static bool read(acl::shared_stream& conn, shared_message& message);
    static bool read_packet(acl::shared_stream& conn, rpc_packet& packet);
    static bool read(acl::shared_stream& conn, size_t len, std::string& out);
};

} // namespace pkv
