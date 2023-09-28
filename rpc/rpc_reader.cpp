//
// Created by shuxin zheng on 2023/9/27.
//

#include "stdafx.h"
#include "rpc_message.h"
#include "rpc_reader.h"

namespace pkv {

bool rpc_reader::read(acl::shared_stream &conn, shared_message &message) {
    rpc_message_hdr hdr { 0, 0, 0, 0, 0 };

    int ret = conn->read(&hdr, sizeof(hdr), true);
    if (ret != sizeof(hdr)) {
        return false;
    }

    if (hdr.oper > (int) message_oper_max) {
        logger_error("Invalid oper=%u", hdr.oper);
        return false;
    }

    if (hdr.klen > MESSAGE_KEY_MAX || hdr.klen == 0) {
        logger_error("Key len invalid, len=%u", hdr.klen);
        return false;
    }

    if (hdr.vlen > MESSAGE_VAL_MAX) {
        logger_error("Value len too long, len=%u", hdr.vlen);
        return false;
    }

    std::string key;
    if (!read(conn, hdr.klen, key)) {
        logger_error("Read key error");
        return false;
    }

    std::string value;
    if (hdr.vlen > 0) {
        if (!read(conn, hdr.vlen, value)) {
            logger_error("Read value error");
            return false;
        }
    }

    message = std::make_shared<kv_message>(key, value, (message_oper_t) hdr.oper);
    return true;
}

bool rpc_reader::read(acl::shared_stream& conn, size_t len, std::string& out) {
    char buf[len + 1];

    if (conn->read(buf, len, true) == -1) {
        logger_error("Read error, len=%zd", len);
        return false;
    }

    out.assign(buf, len);
    return true;
}

} // namespace pkv
