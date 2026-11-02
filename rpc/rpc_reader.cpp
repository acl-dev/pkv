//
// Created by shuxin zheng on 2023/9/27.
//

#include "stdafx.h"
#include "rpc_message.h"
#include "rpc_reader.h"
#include "page_stream.h"
#include "page_wire.h"

namespace pkv {

bool rpc_reader::read(acl::shared_stream &conn, shared_message &message) {
    message.reset();
    rpc_packet packet;
    if (!read_packet(conn, packet) || packet.page) {
        return false;
    }
    message = std::move(packet.message);
    return true;
}

bool rpc_reader::read_packet(acl::shared_stream& conn, rpc_packet& packet) {
    packet = rpc_packet();
    if (!conn || !conn->opened()) return false;
    rpc_message_hdr hdr { 0, 0, 0, 0, 0 };

    int ret = conn->read(&hdr, sizeof(hdr), true);
    if (ret != sizeof(hdr)) {
        return false;
    }

    unsigned database = 0;
    const auto magic = ntohl(hdr.oper);
    const unsigned version = magic == rpc_version_one ? 1 :
        page_wire::version(magic);
    if (version >= 2 && ntohl(hdr.klen) >= 5) {
        if (!page_stream::read_payload(conn,
            reinterpret_cast<const char*>(&hdr), packet.frame)) {
            return false;
        }
        packet.page = true;
        return true;
    }
    const bool versioned = version != 0;
    if (versioned) {
        database = ntohl(hdr.vlen);
        const auto operation = ntohl(hdr.klen);
        const auto key_length = ntohl(hdr.flag);
        const auto value_length = ntohl(hdr.cmd);
        hdr = rpc_message_hdr{operation, key_length, value_length, 0, 0};
    }
    if (hdr.oper < message_oper_set || hdr.oper >= message_oper_max || hdr.flag
        != 0 || hdr.cmd != 0
        || database >= rpc_database_max || (!versioned && hdr.oper ==
            message_oper_ready)
        || (hdr.oper == message_oper_ready && (database != 0 || hdr.klen != 0 ||
            hdr.vlen != 0))) {
        logger_error("Invalid RPC header: oper=%u, database=%u", hdr.oper,
            database);
        return false;
    }

    if (hdr.klen > MESSAGE_KEY_MAX) {
        logger_error("Key len invalid, len=%u", hdr.klen);
        return false;
    }

    if (hdr.vlen > MESSAGE_VAL_MAX || (hdr.oper == message_oper_del && hdr.vlen
        != 0)) {
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

    packet.message = std::make_shared<kv_message>(std::move(key),
        std::move(value), static_cast<message_oper_t>(hdr.oper), database,
        version);
    return true;
}

bool rpc_reader::read(acl::shared_stream& conn, size_t len, std::string& out) {
    if (len && (!conn || !conn->opened())) return false;
    std::string buf(len, '\0');
    if (len == 0) {
        out.clear();
        return true;
    }
    if (conn->read(&buf[0], len, true) != static_cast<int>(len)) {
        logger_error("Read error, len=%zd", len);
        return false;
    }

    out.swap(buf);
    return true;
}

} // namespace pkv
