//
// Created by shuxin zheng on 2023/9/27.
//

#include "stdafx.h"
#include "rpc_message.h"
#include "rpc_sender.h"

namespace pkv {

bool rpc_sender::send(acl::shared_stream &conn, shared_message &message) {
    auto& key = message->get_key();
    auto& val = message->get_value();
    rpc_message_hdr hdr { (unsigned) message->get_oper(),
                          (unsigned) key.size(),
                          (unsigned) val.size(),
                          0,
                          0};

    struct iovec v[3];
    int n = 1;

    v[0].iov_base = &hdr;
    v[0].iov_len = sizeof(hdr);
    if (!key.empty()) {
        v[1].iov_base = (void *) key.c_str();
        v[1].iov_len = key.size();
        n++;

        if (!val.empty()) {
            v[2].iov_base = (void *) val.c_str();
            v[2].iov_len = val.size();
            n++;
        }
    }
    if (conn->writev(v, n) == -1) {
        return false;
    }
    return true;
}

} // namespace pkv