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

// This is the legacy native-endian wire layout, not a versioned network ABI.
static_assert(sizeof(unsigned) == 4 && sizeof(rpc_message_hdr) == 20,
    "unsupported legacy RPC header layout");

// Version 1 header: magic, operation, database, key length, value length.
// All five fields use network byte order. Legacy headers remain unchanged.
constexpr unsigned rpc_version_one = 0x504b5631;
constexpr unsigned rpc_database_max = 65536;
constexpr const char* rpc_upgrade_request = "PKV-REPL 1\r\n";

class rpc_message {
public:
    rpc_message();
    ~rpc_message();
};

} // namespace pkv
