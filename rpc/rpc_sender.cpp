//
// Created by shuxin zheng on 2023/9/27.
//

#include "stdafx.h"
#include "rpc_message.h"
#include "rpc_sender.h"

namespace pkv {

bool rpc_sender::send(acl::shared_stream& conn, kv_message* message) {
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

bool rpc_sender::send(acl::shared_stream& conn,
        const std::vector<kv_message*>& messages) {

    size_t len = 0;

    for (auto it : messages) {
        len += sizeof(rpc_message_hdr);
        len += it->get_key().size();
        len += it->get_value().size();
    }

    if (len == 0) {
        return true;
    }
    return send(conn, messages, len);
}

bool rpc_sender:: send(acl::shared_stream& conn,
        const std::vector<kv_message*>& messages, size_t len) {
    char buf[len + len % 4], *ptr = buf;

    rpc_message_hdr hdr;
    for (auto it : messages) {
        auto& key = it->get_key();
        auto& val = it->get_value();
        hdr.oper = (unsigned) it->get_oper();
        hdr.klen = (unsigned) key.size();
        hdr.vlen = (unsigned) val.size();
        hdr.flag = 0;
        hdr.cmd  = 0;
        memcpy(ptr, &hdr, sizeof(hdr));
        ptr += sizeof(hdr);

        if (!key.empty()) {
            memcpy(ptr, key.c_str(), key.size());
            ptr += key.size();

            if (!val.empty()) {
                memcpy(ptr, val.c_str(), val.size());
                ptr += val.size();
            }
        }
    }
    if (conn->write(buf, ptr - buf) == -1) {
        return false;
    }
    return true;
}

} // namespace pkv
