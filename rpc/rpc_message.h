//
// Created by shuxin zheng on 2023/9/27.
//

#pragma once

namespace pkv {

struct rpc_message_hdr {
    unsigned oper;
    unsigned klen;
    unsigned vlen;
    unsigned flag;
    unsigned cmd;
};

class rpc_message {
public:
    rpc_message();
    ~rpc_message();
};

} // namespace pkv
