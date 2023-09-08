//
// Created by shuxin zheng on 2023/8/1.
//

#include "stdafx.h"
#include "coder/redis_coder.h"
#include "coder/redis_object.h"
#include "dao/json/json_hash.h"

#include "redis_handler.h"
#include "redis_hash.h"

namespace pkv {

#define EQ  !strcasecmp

redis_hash::redis_hash(redis_handler &handler, const redis_object &obj)
: redis_command(handler, obj)
{
    dao_ = new dao::json_hash;
}

redis_hash::~redis_hash() {
    delete dao_;
}

struct hash_handler {
    const char* cmd;
    bool (redis_hash::*func) (redis_coder&);
};

static struct hash_handler handlers[] = {
    { "HSET",       &redis_hash::hset       },
    { "HGET",       &redis_hash::hget       },
    { "HDEL",       &redis_hash::hdel       },
    { "HMSET",      &redis_hash::hmset      },
    { "HMGET",      &redis_hash::hmget      },
    { "HGETALL",    &redis_hash::hgetall    },
    { nullptr,      nullptr                 },
};

bool redis_hash::exec(const char* cmd, redis_coder& result) {
    for (int i = 0; handlers[i].cmd != nullptr; i++) {
        if (EQ(cmd, handlers[i].cmd)) {
            return (this->*(handlers[i].func))(result);
        }
    }

    logger_error("Not support, cmd=%s", cmd);
    return false;
}

bool redis_hash::hset(redis_coder &result) {
    if (obj_.size() < 4) {
        logger_error("Invalid HSET command's size=%zd < 4", obj_.size());
        return false;
    }

    auto key = obj_[1];
    if (key == nullptr || *key == 0) {
        logger_error("key null");
        return false;
    }

    auto name = obj_[2];
    if (name == nullptr || *name == 0) {
        logger_error("name null");
        return false;
    }

    auto value = obj_[3];
    if (value == nullptr || *value == 0) {
        logger_error("value null");
        return false;
    }

    if (!dao_->hset(handler_.get_db(), key, name, value)) {
        return false;
    }

    result.create_object().set_number(1);
    return true;
}

bool redis_hash::hget(redis_coder &result) {
    if (obj_.size() < 3) {
        logger_error("Invalid HGET command's size=%zd < 3", obj_.size());
        return false;
    }

    auto key = obj_[1];
    if (key == nullptr || *key == 0) {
        logger_error("key null");
        return false;
    }

    auto name = obj_[2];
    if (name == nullptr || *name == 0) {
        logger_error("name null");
        return false;
    }

    std::string buff;
    if (!dao_->hget(handler_.get_db(), key, name, buff)) {
        return false;
    }

    //printf(">>hget: [%s]\r\n", buff.c_str());
    result.create_object().set_string(buff);
    return true;
}

bool redis_hash::hdel(redis_coder &result) {
    if (obj_.size() < 3) {
        logger_error("Invalid HDEL params' size=%zd < 3", obj_.size());
        return false;
    }

    auto key = obj_[1];
    if (key == nullptr || *key == 0) {
        logger_error("key null");
        return false;
    }

    auto name = obj_[2];
    if (name == nullptr || *name == 0) {
        logger_error("name null");
        return false;
    }

    int ret = dao_->hdel(handler_.get_db(), key, name);
    result.create_object().set_number(ret);
    return true;
}

bool redis_hash::hmset(redis_coder &result) {
    size_t n = obj_.size();
    if (n < 4 || (n - 4) % 2 != 0) {
        logger_error("Invalid HMSET command's size=%zd", n);
        return false;
    }

    auto key = obj_[1];
    if (key == nullptr || *key == 0) {
        logger_error("key null");
        return false;
    }

    std::map<std::string, std::string> fields;
    for (size_t i = 2; i < n; i += 2) {
        auto name = obj_[i];
        auto value = obj_[i + 1];
        if (name == nullptr || *name == 0) {
            logger_error("name null");
            return false;
        }
        if (value == nullptr || *value == 0) {
            logger_error("value null");
            return false;
        }

        fields[name] = value;
    }

    int ret = dao_->hmset(handler_.get_db(), key, fields);

    result.create_object().set_number(ret);
    return true;
}

bool redis_hash::hmget(redis_coder &result) {
    size_t n = obj_.size();
    if (n < 3) {
        logger_error("Invalid HMGET command's size=%zd", n);
        return false;
    }

    auto key = obj_[1];
    if (key == nullptr || *key == 0) {
        logger_error("key null");
        return false;
    }

    std::vector<std::string> names;
    for (size_t i = 2; i < n; i++) {
        auto name = obj_[i];
        if (name == nullptr || *name == 0) {
            logger_error("name null");
            return false;
        }
        names.emplace_back(name);
    }

    std::map<std::string, std::string> fields;
    if (!dao_->hmget(handler_.get_db(), key, names, fields)) {
        return false;
    }

    auto& obj = result.create_object();
    for (const auto& it : fields) {
        obj.create_child().set_string(it.first, true)
            .create_child().set_string(it.second);
    }
    return true;
}

bool redis_hash::hgetall(redis_coder &result) {
    if (obj_.size() < 2) {
        logger_error("Invalid HGETALL command's size=%zd < 2", obj_.size());
        return false;
    }

    auto key = obj_[1];
    if (key == nullptr || *key == 0) {
        logger_error("key null");
        return false;
    }

    if (!dao_->hgetall(handler_.get_db(), key)) {
        logger_error("dao.hgetall error, key=%s", key);
        return false;
    }

    auto& obj = result.create_object();

    auto& fields = dao_->get_fields();
    if (fields.empty()) {
        obj.set_type(REDIS_OBJ_EMPTY);
        return true;
    }
    for (const auto& it : fields) {
        obj.create_child().set_string(it.first, true)
            .create_child().set_string(it.second);
    }
    return true;
}

} // namespace pkv
