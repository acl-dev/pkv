//
// Created by shuxin zheng on 2023/9/28.
//

#include "stdafx.h"
#include "rpc/rpc_reader.h"
#include "rpc/password_handshake.h"
#include "db/database_pool.h"
#include "coder/record_codec.h"
#include "cluster_client.h"
#include <utility>

namespace pkv {

namespace {
bool native_value(const std::string& value) {
    record_header header;
    return default_record_codec()->stored_header(value, header) == codec_status::ok;
}

bool native_batch(const page_batch& batch) {
    for (const auto& root : batch.roots()) {
        if (root.action == db::update_action::write && !native_value(root.value))
            return false;
    }
    return true;
}

bool native_group(const exec_batch& group) {
    for (const auto& batch : group.batches()) {
        if (!native_batch(*batch)) return false;
    }
    for (const auto& entry : group.entries()) {
        if (entry.pages()) {
            if (!native_batch(*entry.pages())) return false;
        } else {
            const auto& move = *entry.move();
            if (!native_value(move.source_root) ||
                (move.target_found && !native_value(move.target_root)))
                return false;
        }
    }
    return true;
}
} // namespace

cluster_client::cluster_client(std::string addr, shared_db database,
    std::shared_ptr<database_pool> databases, std::string password)
: addr_(std::move(addr))
, password_(std::move(password))
, db_(std::move(database))
, databases_(std::move(databases))
{
    conn_ = std::make_shared<acl::socket_stream>();
}

bool cluster_client::open() {
    conn_->close();
    negotiated_ = false;
    wire_version_ = 0;
    pages_.reset(); executions_.reset(); ordered_.reset();
    requested_version_ = next_version_;
    next_version_ = 5;
    const char* request = requested_version_ == 5 ? "PKV-REPL 5\r\n" :
        requested_version_ == 4 ? "PKV-REPL 4\r\n" :
        requested_version_ == 3 ? "PKV-REPL 3\r\n" :
        requested_version_ == 2 ?
        "PKV-REPL 2\r\n" : "PKV-REPL 1\r\n";
    const auto length = std::strlen(request);
    if (conn_->open(addr_.c_str(), 30, -1)) {
        if (!send_password(conn_, password_)) {
            requested_version_ = 0; next_version_ = 5;
            conn_->close();
            return false;
        }
        if (conn_->write(request, length) == static_cast<int>(length)) {
            logger("Connect %s ok!", addr_.c_str());
            return true;
        }
    }
    logger_error("Connect %s error %s", addr_.c_str(), acl::last_serror());
    conn_->close();
    return false;
}

void cluster_client::run() {
    if (!conn_->opened()) reopen();
    while (true) {
        rpc_packet packet;
        if (!rpc_reader::read_packet(conn_, packet) ||
            !apply_packet(std::move(packet))) {
            if (!negotiated_ && requested_version_ > 1) {
                next_version_ = requested_version_ - 1;
            }
            pages_.reset(); executions_.reset(); ordered_.reset();
            conn_->close();
            ::sleep(1);
            reopen();
        }
    }
}

void cluster_client::reopen() {
    while (!open()) {
        ::sleep(1);
    }
}

shared_db cluster_client::select(unsigned database) {
    if (database == 0) {
        return db_;
    }
    if (!negotiated_ || var_cfg_cluster_mode || !databases_ ||
        database >= databases_->size()) {
        logger_error("Invalid replica database=%u", database);
        return nullptr;
    }
    return databases_->get(database);
}

bool cluster_client::apply_packet(rpc_packet packet) {
    if (!packet.page) {
        if (executions_.active() || ordered_.active()) return false;
        return apply(packet.message);
    }
    if (packet.message || !negotiated_ || wire_version_ < 2 ||
        (wire_version_ == 5 ? (packet.frame.protocol != 4 &&
            packet.frame.protocol != 5) :
            packet.frame.protocol != wire_version_)) {
        return false;
    }
    if (executions_.active() || ordered_.active() ||
        packet.frame.operation == page_operation::exec_begin) {
        if (wire_version_ < 3 || pages_.active()) return false;
        std::shared_ptr<const exec_batch> group;
        const auto status = wire_version_ >= 4 ?
            ordered_.accept(std::move(packet.frame), group) :
            executions_.accept(std::move(packet.frame), group);
        if (status == exec_receiver::result::error) return false;
        if (status != exec_receiver::result::complete) return true;
        if (!native_group(*group)) {
            logger_error("Replica group contains an unsupported root");
            return false;
        }
        unsigned database;
        if (!group->database(database)) {
            if (var_cfg_cluster_mode || !databases_ ||
                databases_->get(0) != db_) return false;
            return group->apply(*databases_) == db::batch_status::ok;
        }
        const auto target = select(database);
        return target && group->apply(*target) == db::batch_status::ok;
    }
    // The receiver checks that subsequent frames use the BEGIN database.
    if (!pages_.active() && !select(packet.frame.database)) return false;
    std::shared_ptr<const page_batch> batch;
    const auto status = pages_.accept(std::move(packet.frame), batch);
    if (status == page_receiver::result::error) {
        return false;
    }
    if (status != page_receiver::result::complete) return true;
    if (!native_batch(*batch)) {
        logger_error("Replica page batch contains an unsupported root");
        return false;
    }
    const auto selected = select(batch->database());
    return selected && batch->apply(*selected) == db::batch_status::ok;
}

bool cluster_client::apply(const shared_message& message) {
    if (!message || pages_.active() || executions_.active() ||
        ordered_.active()) {
        return false;
    }
    const auto version = message->get_protocol();
    if (message->get_oper() == message_oper_ready) {
        if (negotiated_ || (version < 1 || version > 5) ||
            version > requested_version_ ||
            message->get_database() != 0 || !message->get_key().empty() ||
            !message->get_value().empty()) {
            return false;
        }
        negotiated_ = true;
        wire_version_ = version;
        return true;
    }
    if (version > 5 || (version >= 2 && !negotiated_) ||
        (negotiated_ && version != wire_version_)) {
        return false;
    }
    const auto selected = select(message->get_database());
    if (!selected) {
        return false;
    }
    switch (message->get_oper()) {
    case message_oper_set:
        if (!native_value(message->get_value())) {
            logger_error("Replica SET contains an unsupported root");
            return false;
        }
        return selected->replica_set(message->get_key(), message->get_value());
    case message_oper_del:
        return selected->replica_erase(message->get_key());
    case message_oper_get:
        return true;
    default:
        return false;
    }
}

} // namespace pkv
