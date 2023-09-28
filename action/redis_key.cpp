//
// Created by shuxin zheng on 2023/7/31.
//

#include "stdafx.h"
#include "db/db_cursor.h"
#include "dao/json/json_key.h"

#include "redis_handler.h"
#include "redis_key.h"

namespace pkv {

#define EQ  !strcasecmp

redis_key::redis_key(redis_handler& handler, const redis_object& obj)
: redis_command(handler, obj)
{
    dao_ = new dao::json_key;
}

redis_key::~redis_key() {
    delete dao_;
}

struct key_handler {
    const char* cmd;
    bool check_slot;
    bool (redis_key::*func) (redis_coder&);
};

static struct key_handler handlers[] = {
    { "DEL",        true,   &redis_key::del         },
    { "TYPE",       true,   &redis_key::type        },
    { "EXPIRE",     true,   &redis_key::expire      },
    { "TTL",        true,   &redis_key::ttl         },

    { nullptr,      false, nullptr            },
};

bool redis_key::exec(const char* cmd, redis_coder& result) {
    for (int i = 0; handlers[i].cmd != nullptr; i++) {
        if (EQ(cmd, handlers[i].cmd)) {
            if (this->check_cluster_mode(handlers[i].check_slot)) {
                CHECK_AND_REDIRECT(cmd, obj_, var_cfg_service_addr);
            }
            return (this->*(handlers[i].func))(result);
        }
    }

    logger_error("Not support, cmd=%s", cmd);
    return false;
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

    if (!dao_->del(handler_.get_db(), key)) {
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
    if (!dao_->type(handler_.get_db(), key, buff)) {
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

    char* end;
    int n = (int) std::strtol(ptr, &end, 10);
    if (*end != 0) {
        logger_error("Invalid expire=%s, left=%s", ptr, end);
        return false;
    }

    if (!dao_->expire(handler_.get_db(), key, n)) {
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
    if (!dao_->ttl(handler_.get_db(), key, n)) {
        result.create_object().set_number(-2);
    } else {
        result.create_object().set_number(n);
    }
    return true;
}

bool redis_key::scan(db_cursor& cursor, redis_coder& result) {
    if (obj_.size() < 2) {
        logger_error("Invalid SCAN params' size=%zd", obj_.size());
        return false;
    }

    char* end;
    int ncursor = (int) std::strtol(obj_[1], &end, 10);
    if (*end != 0) {
        logger_error("Invalid ncursor=%s, left=%s", obj_[1], end);
        return false;
    }
    if (ncursor <= 0) {  // Scan again.
        cursor.reset();
    }

    std::string pattern;
    size_t count = 10;

    if (obj_.size() == 4) {
        if (strcasecmp(obj_[2], "MATCH") == 0) {
            pattern = obj_[3];
        } else if (strcasecmp(obj_[2], "COUNT") == 0) {
            count = (size_t) std::strtol(obj_[3], &end, 10);
            if (*end != 0) {
                logger_error("Invalid COUNT=%s, left=%s", obj_[3], end);
                return false;
            }
        }
    }

    if (obj_.size() == 6) {
        if (strcasecmp(obj_[2], "MATCH") == 0) {
            pattern = obj_[3];
        }
        if (strcasecmp(obj_[4], "COUNT") == 0) {
            count = (size_t) std::strtol(obj_[5], &end, 10);
            if (*end != 0) {
                logger_error("Invalid COUNT=%s, left=%s", obj_[5], end);
                return false;
            }
        }
    }

    std::vector<std::string> keys;
    if (!dao_->scan(handler_.get_db(), cursor, keys, count)) {
        logger_error("scan error");
        return false;
    }

    if (keys.empty()) {
        auto& o = result.create_object();
        o.create_child().set_string("0");
        o.create_child().set_type(REDIS_OBJ_EMPTY);
        return true;
    }

    auto& o = result.create_object();
    o.create_child().set_string(std::to_string(keys.size()));

    auto& children = o.create_child();
    for (auto& it : keys) {
        children.create_child().set_string(it);
    }
    return true;
}

} // namespace pkv
