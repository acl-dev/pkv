//
// Created by shuxin zheng on 2023/9/28.
//

#include "stdafx.h"
#include "rpc/rpc_reader.h"
#include "cluster_client.h"

#include <utility>

namespace pkv {

cluster_client::cluster_client(std::string addr, shared_db db)
: addr_(std::move(addr))
, db_(std::move(db))
{
    conn_ = std::make_shared<acl::socket_stream>();
}

bool cluster_client::open() {
    if (conn_->open(addr_.c_str(), 30, -1)) {
        logger("Connect %s ok!", addr_.c_str());
        return true;
    }

    logger_error("Connect %s error %s", addr_.c_str(), acl::last_serror());
    return false;
}

void cluster_client::run() {
    while (true) {
        shared_message message;
        if (rpc_reader::read(conn_, message)) {
            handle_message(message);
        } else {
            ::sleep(1);
            reopen();
        }
    }
}

void cluster_client::reopen() {
    conn_->close();
    while (true) {
        if (conn_->open(addr_.c_str(), 30, -1)) {
            break;
        }
        ::sleep(1);
    }
}

void cluster_client::handle_message(shared_message &message) {
    auto& key = message->get_key();
    auto& val = message->get_value();

    switch (message->get_oper()) {
    case message_oper_set:
        if (!db_->dbset(key, val)) {
            logger_error("dbset error, key=%s, val=%s", key.c_str(), val.c_str());
        } else {
        //    printf(">>>%s: set ok, key=%s, val=%s\r\n", __func__, key.c_str(),
        //           val.c_str());
        }
        break;
    case message_oper_del:
        if (!db_->dbdel(key)) {
            logger_error("dbdel error, key=%s", key.c_str());
        } else {
        //    printf(">>>%s: del ok, key=%s, val=%s\r\n", __func__, key.c_str(),
        //           val.c_str());
        }
        break;
    case message_oper_get:
        //printf(">>>%s: get, key=%s, val=%s\r\n", __func__, key.c_str(), val.c_str());
        break;
    default:
        break;
    }
}

} // namespace pkv
