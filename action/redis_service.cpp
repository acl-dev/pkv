//
// Created by shuxin zheng on 2023/8/18.
//

#include "stdafx.h"
#include <algorithm>
#include "redis_key.h"
#include "redis_string.h"
#include "redis_hash.h"
#include "redis_cluster.h"
#include "redis_server.h"
#include "redis_service.h"

namespace pkv {

redis_service::redis_service() {
    handlers_["DEL"] = key_handler;
    handlers_["TYPE"] = key_handler;
    handlers_["EXPIRE"] = key_handler;
    handlers_["TTL"] = key_handler;
    handlers_["SET"] = string_handler;
    handlers_["GET"] = string_handler;
    handlers_["HSET"] = hash_handler;
    handlers_["HGET"] = hash_handler;
    handlers_["HDEL"] = hash_handler;
    handlers_["HMSET"] = hash_handler;
    handlers_["HMGET"] = hash_handler;
    handlers_["HGETALL"] = hash_handler;
    handlers_["CONFIG"] = server_handler;
    handlers_["CLUSTER"] = cluster_handler;
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

bool redis_service::key_handler(redis_handler& handler, const char* cmd,
     const redis_object& obj, redis_coder& result) {
    redis_key redis(handler, obj);
    return redis.exec(cmd, result);
}

bool redis_service::string_handler(redis_handler& handler, const char* cmd,
     const redis_object& obj, redis_coder& result) {
    redis_string redis(handler, obj);
    return redis.exec(cmd, result);
}

bool redis_service::hash_handler(redis_handler& handler, const char* cmd,
     const redis_object& obj, redis_coder& result) {
    redis_hash redis(handler, obj);
    return redis.exec(cmd, result);
}

bool redis_service::server_handler(redis_handler& handler, const char* cmd,
     const redis_object& obj, redis_coder& result) {
    redis_server redis(handler, obj);
    return redis.exec(cmd, result);
}

bool redis_service::cluster_handler(redis_handler& handler, const char* cmd,
     const redis_object& obj, redis_coder& result) {
    redis_cluster redis(handler, obj);
    return redis.exec(cmd, result);
}

} // namespace pkv
