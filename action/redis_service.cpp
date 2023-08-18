//
// Created by shuxin ¡¡¡¡zheng on 2023/8/18.
//

#include "stdafx.h"
#include "redis_key.h"
#include "redis_string.h"
#include "redis_hash.h"
#include "redis_server.h"
#include "redis_service.h"

namespace pkv {

redis_service::redis_service() {
    handlers_["SET"] = cmd_set;
    handlers_["GET"] = cmd_get;
    handlers_["DEL"] = cmd_del;
    handlers_["TYPE"] = cmd_type;
    handlers_["HSET"] = cmd_hset;
    handlers_["HGET"] = cmd_hget;
    handlers_["HDEL"] = cmd_hdel;
    handlers_["HMSET"] = cmd_hmset;
    handlers_["HMGET"] = cmd_hmget;
    handlers_["HGETAll"] = cmd_hgetall;
    handlers_["CONFIG"] = cmd_config;
}

redis_handler_t * redis_service::get_handler(const char *cmd) {
    std::string key(cmd);
    std::transform(key.begin(), key.end(), key.begin(), ::toupper);

    auto it = handlers_.find(key);
    if (it == handlers_.end()) {
	return nullptr;
    }

    return &it->second;
}

bool redis_service::cmd_set(redis_handler& handler, const redis_object& obj,
     redis_coder& result) {
    redis_string redis(handler, obj);
    return redis.set(result);
}

bool redis_service::cmd_get(redis_handler& handler, const redis_object& obj,
     redis_coder& result) {
    redis_string redis(handler, obj);
    return redis.get(result);
}

bool redis_service::cmd_del(redis_handler& handler, const redis_object& obj,
     redis_coder& result) {
    redis_key redis(handler, obj);
    return redis.del(result);
}

bool redis_service::cmd_type(redis_handler& handler, const redis_object& obj,
     redis_coder& result) {
    redis_key redis(handler, obj);
    return redis.type(result);
}

bool redis_service::cmd_hset(redis_handler& handler, const redis_object& obj,
     redis_coder& result) {
    redis_hash redis(handler, obj);
    return redis.hset(result);
}

bool redis_service::cmd_hget(redis_handler& handler, const redis_object& obj,
     redis_coder& result) {
    redis_hash redis(handler, obj);
    return redis.hget(result);
}

bool redis_service::cmd_hdel(redis_handler& handler, const redis_object& obj,
     redis_coder& result) {
    redis_hash redis(handler, obj);
    return redis.hdel(result);
}

bool redis_service::cmd_hmset(redis_handler& handler, const redis_object& obj,
     redis_coder& result) {
    redis_hash redis(handler, obj);
    return redis.hmset(result);
}

bool redis_service::cmd_hmget(redis_handler& handler, const redis_object& obj,
     redis_coder& result) {
    redis_hash redis(handler, obj);
    return redis.hmget(result);
}

bool redis_service::cmd_hgetall(redis_handler& handler, const redis_object& obj,
     redis_coder& result) {
    redis_hash redis(handler, obj);
    return redis.hgetall(result);
}

bool redis_service::cmd_config(redis_handler& handler, const redis_object& obj,
     redis_coder& result) {
    redis_server redis(handler, obj);
    return redis.config(result);
}

} // namespace pkv
