//
// Created by shuxin ¡¡¡¡zheng on 2023/7/31.
//

#include "stdafx.h"
#include "coder/redis_coder.h"
#include "dao/key.h"

#include "redis_handler.h"
#include "redis_key.h"

namespace pkv {

#define EQ  !strcasecmp

redis_key::redis_key(redis_handler& handler, const redis_object& obj)
: redis_command(handler, obj)
{
}

bool redis_key::exec(const char* cmd, redis_coder& result) {
    if (EQ(cmd, "DEL")) {
        return del(result);
    } else if (EQ(cmd, "TYPE")) {
        return type(result);
    } else if (EQ(cmd, "EXPIRE")) {
        return expire(result);
    } else if (EQ(cmd, "TTL")) {
        return ttl(result);
    } else {
        return false;
    }
}

bool redis_key::del(redis_coder& result) {
    if (obj_.size() < 2) {
        logger_error("invalid SET params' size=%zd", obj_.size());
        return false;
    }

    auto key = obj_[1];
    if (key == nullptr || *key == 0) {
        logger_error("key null");
        return false;
    }

    dao::key dao;
    if (!dao.del(handler_.get_db(), key)) {
        logger_error("db del error, key=%s", key);
        return false;
    }

    result.create_object().set_number(1);
    return true;
}

bool redis_key::type(redis_coder& result) {
    if (obj_.size() < 2) {
        logger_error("invalid TYPE params' size=%zd", obj_.size());
        return false;
    }

    auto key = obj_[1];
    if (key == nullptr || *key == 0) {
        logger_error("key null");
        return false;
    }

    std::string buff;
    dao::key dao;
    if (!dao.type(handler_.get_db(), key, buff)) {
        return false;
    }

    result.create_object().set_status(buff);
    return true;
}

bool redis_key::expire(pkv::redis_coder &result) {
    if (obj_.size() < 3) {
        logger_error("invalid EXPIRE params' size=%zd", obj_.size());
        return false;
    }

    auto key = obj_[1];
    if (key == nullptr || *key == 0) {
        logger_error("key null");
        return false;
    }
    auto ptr = obj_[2];
    if (ptr == nullptr || *ptr == 0) {
        logger_error("expire time null");
        return false;
    }
    int n = std::atoi(ptr);
    dao::key dao;
    if (!dao.expire(handler_.get_db(), key, n)) {
        result.create_object().set_number(0);
    } else {
        result.create_object().set_number(1);
    }
    return true;
}

bool redis_key::ttl(pkv::redis_coder &result) {
    if (obj_.size() < 2) {
        logger_error("invalid EXPIRE params' size=%zd", obj_.size());
        return false;
    }

    auto key = obj_[1];
    if (key == nullptr || *key == 0) {
        logger_error("key null");
        return false;
    }

    int n;
    dao::key dao;
    if (!dao.ttl(handler_.get_db(), key, n)) {
        result.create_object().set_number(-2);
    } else {
        result.create_object().set_number(n);
    }
    return true;
}

bool redis_key::scan(std::string& scan_key, redis_coder& result) {
    if (obj_.size() < 2) {
        logger_error("invalid SCAN params' size=%zd", obj_.size());
        return false;
    }

    int cursor = std::atoi(obj_[1]);
    if (cursor <= 0) {
        scan_key.clear();
    }

    std::string pattern;
    size_t count = 10;

    if (obj_.size() == 4) {
        if (strcasecmp(obj_[2], "MATCH") == 0) {
            pattern = obj_[3];
        } else if (strcasecmp(obj_[2], "COUNT") == 0) {
            count = (size_t) std::atoi(obj_[3]);
        }
    }

    if (obj_.size() == 6) {
        if (strcasecmp(obj_[2], "MATCH") == 0) {
            pattern = obj_[3];
        }
        if (strcasecmp(obj_[4], "COUNT") == 0) {
            count = (size_t) std::atoi(obj_[5]);
        }
    }

    std::vector<std::string> keys;
    dao::key dao;
    if (!dao.scan(handler_.get_db(), scan_key, keys, count)) {
        logger_error("scan error");
        return false;
    }

    if (keys.empty()) {
        auto& o = result.create_object();
        o.create_child().set_string("0");
        o.create_child().create_empty();
        return true;
    }

    scan_key = keys[keys.size() - 1]; // Save the last key.
    auto& o = result.create_object();
    o.create_child().set_string(std::to_string(keys.size()));

    auto& children = o.create_child();
    for (auto& it : keys) {
        children.create_child().set_string(it);
    }
    return true;
}

} // namespace pkv