//
// Created by zsx on 2023/7/23.
//

#pragma once

#include <vector>
#include "coder/redis_coder.h"
#include "db/db.h"

namespace pkv {

class redis_service;
class redis_object;
class redis_ocache;
class db_cursor;

class redis_handler {
public:
    explicit redis_handler(redis_service& service, shared_db& db,
   	    redis_coder& parser, acl::socket_stream& conn);

    ~redis_handler();

    bool handle();

    NODISCARD redis_ocache& get_cache() const {
        return parser_.get_cache();
    }

    NODISCARD redis_coder& get_coder() {
        return coder_;
    }

    NODISCARD shared_db& get_db() {
        return db_;
    }

private:
    redis_service& service_;
    shared_db& db_;
    redis_coder& parser_;
    acl::socket_stream& conn_;
    redis_coder builder_;
    redis_coder coder_;
    db_cursor* cursor_ = nullptr;

    bool handle_one(const redis_object& obj);
};

} // namespace pkv