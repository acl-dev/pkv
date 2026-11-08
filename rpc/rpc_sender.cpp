//
// Created by shuxin zheng on 2023/9/27.
//

#include "stdafx.h"
#include <climits>
#include <stdexcept>
#include "rpc_message.h"
#include "rpc_sender.h"
#include "page_stream.h"
#include "page_wire.h"
#include "exec_batch.h"
#include "exec_stream.h"

namespace pkv {
namespace {

bool message_size(const kv_message* message, size_t& size, unsigned version) {
    if (!message || version > 5) {
        return false;
    }
    if (message->get_oper() == message_oper_exec) {
        if (!message->get_exec() || version < message->get_exec()->protocol())
            return false;
        size = message->wire_bytes(); return true;
    }
    if (message->get_oper() == message_oper_pages) {
        if (!message->get_pages() ||
            version < message->get_pages()->required_protocol()) {
            return false;
        }
        size = message->wire_bytes();
        return true;
    }
    if (message->get_oper() < message_oper_set ||
        message->get_oper() >= message_oper_max ||
        (!version && (message->get_database() != 0 ||
            message->get_oper() == message_oper_ready)) ||
        message->get_database() >= rpc_database_max ||
        (message->get_oper() == message_oper_ready &&
            (message->get_database() != 0 || !message->get_key().empty() ||
                !message->get_value().empty())) ||
        message->get_key().size() > MESSAGE_KEY_MAX ||
        message->get_value().size() > MESSAGE_VAL_MAX ||
        (message->get_oper() == message_oper_del &&
            !message->get_value().empty())) {
        return false;
    }
    size = message->wire_bytes();
    return true;
}

bool batch_size(const std::vector<kv_message*>& messages, size_t& size,
    unsigned version) {
    size = 0;
    for (const auto message : messages) {
        size_t current;
        if (!message_size(message, current, version) ||
            current > static_cast<size_t>(INT_MAX) - size) {
            return false;
        }
        size += current;
    }
    return true;
}

rpc_message_hdr header_for(const kv_message& message, unsigned version) {
    const auto key = static_cast<unsigned>(message.get_key().size());
    const auto value = static_cast<unsigned>(message.get_value().size());
    if (version) {
        return {htonl(version >= 2 ? page_wire::magic_for(version) :
            rpc_version_one),
            htonl(message.get_oper()), htonl(message.get_database()),
            htonl(key), htonl(value)};
    }
    return {static_cast<unsigned>(message.get_oper()), key, value, 0, 0};
}

bool write_normal(acl::shared_stream& conn, const kv_message& message,
    unsigned version) {
    auto header = header_for(message, version);
    const auto& key = message.get_key();
    const auto& value = message.get_value();
    iovec vectors[3];
    vectors[0].iov_base = &header;
    vectors[0].iov_len = sizeof(header);
    int count = 1;
    if (!key.empty()) {
        vectors[count].iov_base = const_cast<char*>(key.data());
        vectors[count++].iov_len = key.size();
    }
    if (!value.empty()) {
        vectors[count].iov_base = const_cast<char*>(value.data());
        vectors[count++].iov_len = value.size();
    }
    return conn->writev(vectors, count) ==
        static_cast<int>(message.wire_bytes());
}

bool send_frames(acl::shared_stream& conn,
    const std::vector<kv_message*>& messages, size_t capacity, unsigned version)
        {
    size_t size;
    if (!batch_size(messages, size, version) || capacity < size) {
        return false;
    }
    if (!size) {
        return true;
    }
    try {
        if (version >= 2) {
            for (size_t i = 0; i < messages.size();) {
                const auto* message = messages[i];
                if (message->get_oper() == message_oper_pages) {
                    const page_batch* run[16];
                    size_t count = 0;
                    do {
                        run[count++] = messages[i++]->get_pages().get();
                    } while (count < sizeof(run) / sizeof(run[0]) &&
                        i < messages.size() &&
                        messages[i]->get_oper() == message_oper_pages);
                    if (!page_stream::send_run(conn, run, count, version))
                        return false;
                    continue;
                }
                const bool sent = message->get_oper() == message_oper_exec ?
                    exec_stream::send(conn, *message->get_exec(), version) :
                    write_normal(conn, *message, version);
                if (!sent) return false;
                ++i;
            }
            return true;
        }
        std::vector<char> buffer(size);
        char* next = buffer.data();
        for (const auto message : messages) {
            const auto header = header_for(*message, version);
            memcpy(next, &header, sizeof(header));
            next += sizeof(header);
            for (const auto* bytes : {&message->get_key(),
                &message->get_value()}) {
                if (!bytes->empty()) {
                    memcpy(next, bytes->data(), bytes->size());
                    next += bytes->size();
                }
            }
        }
        return conn->write(buffer.data(), buffer.size()) ==
            static_cast<int>(size);
    } catch (const std::bad_alloc&) {
        return false;
    } catch (const std::length_error&) {
        return false;
    }
}

} // namespace

bool rpc_sender::send(acl::shared_stream& conn, kv_message* message) {
    size_t size;
    return message_size(message, size, 0) && write_normal(conn, *message, 0);
}

bool rpc_sender::send(acl::shared_stream& conn,
    const std::vector<kv_message*>& messages) {
    return send_frames(conn, messages, static_cast<size_t>(INT_MAX), 0);
}

bool rpc_sender::send(acl::shared_stream& conn,
    const std::vector<kv_message*>& messages, size_t capacity) {
    return send_frames(conn, messages, capacity, 0);
}

bool rpc_sender::send_versioned(acl::shared_stream& conn,
    const std::vector<kv_message*>& messages, unsigned version) {
    if (!version || version > 5) {
        return false;
    }
    return send_frames(conn, messages, static_cast<size_t>(INT_MAX), version);
}

} // namespace pkv
