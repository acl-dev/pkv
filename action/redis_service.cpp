//
// Created by shuxin zheng on 2023/8/18.
//

#include "stdafx.h"
#include <algorithm>
#include "redis_key.h"
#include "redis_string.h"
#include "redis_hash.h"
#include "redis_list.h"
#include "redis_set.h"
#include "redis_scan.h"
#include "redis_zset.h"
#include "redis_cluster.h"
#include "redis_server.h"
#include "redis_hyperloglog.h"
#include "redis_geo.h"
#include "redis_service.h"
#include "common/acl_file.h"
#include "common/command_catalog.h"

namespace pkv {

redis_service::redis_service() {
    commands_.reserve(128);
    add("SUBSCRIBE", pubsub_handler, 2, 0, redis_command_kind::pubsub);
    add("UNSUBSCRIBE", pubsub_handler, 1, 0, redis_command_kind::pubsub);
    add("PSUBSCRIBE", pubsub_handler, 2, 0, redis_command_kind::pubsub);
    add("PUNSUBSCRIBE", pubsub_handler, 1, 0, redis_command_kind::pubsub);
    add("SSUBSCRIBE", pubsub_handler, 2, 0, redis_command_kind::pubsub);
    add("SUNSUBSCRIBE", pubsub_handler, 1, 0, redis_command_kind::pubsub);
    add("PUBLISH", pubsub_handler, 3, 3, redis_command_kind::pubsub);
    add("SPUBLISH", pubsub_handler, 3, 3, redis_command_kind::pubsub);
    add("PUBSUB", pubsub_handler, 2, 0, redis_command_kind::pubsub);
    add("BLPOP", list_handler, 3, 0, redis_command_kind::blocking);
    add("BRPOP", list_handler, 3, 0, redis_command_kind::blocking);
    add("BLMPOP", list_handler, 5, 0, redis_command_kind::blocking);
    add("BLMOVE", list_handler, 6, 6, redis_command_kind::blocking);
    add("BRPOPLPUSH", list_handler, 4, 4, redis_command_kind::blocking);
    add("MULTI", nullptr, 1, 1);
    add("EXEC", nullptr, 1, 1, redis_command_kind::exec);
    add("DISCARD", nullptr, 1, 1);
    add("WATCH", nullptr, 2, 0);
    add("UNWATCH", nullptr, 1, 1);
    add("SET", string_handler, 3, 0);
    add("GET", string_handler, 2, 2);
    add("PFADD", hll_handler, 2, 0);
    add("PFCOUNT", hll_handler, 2, 0);
    add("PFMERGE", hll_handler, 2, 0);
    add("DIGEST", string_handler, 2, 2);
    add("DELEX", string_handler, 2, 0);
    add("LCS", string_handler, 3, 0);
    add("MGET", string_handler, 2, 0);
    add("MSET", string_handler, 3, 0);
    add("MSETEX", string_handler, 4, 0);
    add("MSETNX", string_handler, 3, 0);
    add("EXISTS", key_handler, 2, 0);
    add("DEL", key_handler, 2, 0);
    add("TYPE", key_handler, 2, 2);
    add("EXPIRE", key_handler, 3, 0);
    add("TTL", key_handler, 2, 2);
    add("PTTL", key_handler, 2, 2);
    add("PEXPIRE", key_handler, 3, 0);
    add("EXPIREAT", key_handler, 3, 0);
    add("PEXPIREAT", key_handler, 3, 0);
    add("EXPIRETIME", key_handler, 2, 2);
    add("PEXPIRETIME", key_handler, 2, 2);
    add("PERSIST", key_handler, 2, 2);
    add("RENAME", key_handler, 3, 3);
    add("RENAMENX", key_handler, 3, 3);
    add("HSET", hash_handler, 4, 0);
    add("HGET", hash_handler, 3, 3);
    add("HGETDEL", hash_handler, 5, 0);
    add("HDEL", hash_handler, 3, 0);
    add("HMSET", hash_handler, 4, 0);
    add("HMGET", hash_handler, 3, 0);
    add("HGETALL", hash_handler, 2, 2);
    add("HSETNX", hash_handler, 4, 4);
    add("HINCRBY", hash_handler, 4, 4);
    add("HINCRBYFLOAT", hash_handler, 4, 4);
    add("HLEN", hash_handler, 2, 2);
    add("HSTRLEN", hash_handler, 3, 3);
    add("HEXISTS", hash_handler, 3, 3);
    add("HKEYS", hash_handler, 2, 2);
    add("HVALS", hash_handler, 2, 2);
    add("HRANDFIELD", hash_handler, 2, 0);
    add("LPUSH", list_handler, 3, 0);
    add("RPUSH", list_handler, 3, 0);
    add("LPUSHX", list_handler, 3, 0);
    add("RPUSHX", list_handler, 3, 0);
    add("LPOP", list_handler, 2, 3);
    add("RPOP", list_handler, 2, 3);
    add("LLEN", list_handler, 2, 2);
    add("LRANGE", list_handler, 4, 4);
    add("LINDEX", list_handler, 3, 3);
    add("LSET", list_handler, 4, 4);
    add("LTRIM", list_handler, 4, 4);
    add("LINSERT", list_handler, 5, 5);
    add("LREM", list_handler, 4, 4);
    add("LPOS", list_handler, 3, 0);
    add("LMOVE", list_handler, 5, 5);
    add("RPOPLPUSH", list_handler, 3, 3);
    add("LMPOP", list_handler, 4, 0);
    add("SADD", set_handler, 3, 0);
    add("SREM", set_handler, 3, 0);
    add("SCARD", set_handler, 2, 2);
    add("SISMEMBER", set_handler, 3, 3);
    add("SMISMEMBER", set_handler, 3, 0);
    add("SMEMBERS", set_handler, 2, 2);
    add("SMOVE", set_handler, 4, 4);
    add("SPOP", set_handler, 2, 0);
    add("SRANDMEMBER", set_handler, 2, 0);
    add("SINTERCARD", set_handler, 3, 0);
    add("SUNION", set_handler, 2, 0);
    add("SINTER", set_handler, 2, 0);
    add("SDIFF", set_handler, 2, 0);
    add("SUNIONSTORE", set_handler, 3, 0);
    add("SINTERSTORE", set_handler, 3, 0);
    add("SDIFFSTORE", set_handler, 3, 0);
    add("GEORADIUS", geo_handler, 6, 0);
    add("GEORADIUS_RO", geo_handler, 6, 0);
    add("GEORADIUSBYMEMBER", geo_handler, 5, 0);
    add("GEORADIUSBYMEMBER_RO", geo_handler, 5, 0);
    add("GEOSEARCH", geo_handler, 7, 0);
    add("GEOSEARCHSTORE", geo_handler, 8, 0);
    add("GEOADD", geo_handler, 5, 0);
    add("GEOPOS", geo_handler, 2, 0);
    add("GEOHASH", geo_handler, 2, 0);
    add("GEODIST", geo_handler, 4, 0);
    add("ZADD", zset_handler, 4, 0);
    add("ZINCRBY", zset_handler, 4, 4);
    add("ZREM", zset_handler, 3, 0);
    add("ZRANDMEMBER", zset_handler, 2, 0);
    add("ZCARD", zset_handler, 2, 2);
    add("ZSCORE", zset_handler, 3, 3);
    add("ZMSCORE", zset_handler, 3, 0);
    add("ZRANGE", zset_handler, 4, 0);
    add("ZREVRANGE", zset_handler, 4, 0);
    add("ZRANGEBYSCORE", zset_handler, 4, 0);
    add("ZREVRANGEBYSCORE", zset_handler, 4, 0);
    add("ZRANGEBYLEX", zset_handler, 4, 0);
    add("ZREVRANGEBYLEX", zset_handler, 4, 0);
    add("ZCOUNT", zset_handler, 4, 4);
    add("ZLEXCOUNT", zset_handler, 4, 4);
    add("ZREMRANGEBYRANK", zset_handler, 4, 4);
    add("ZREMRANGEBYSCORE", zset_handler, 4, 4);
    add("ZREMRANGEBYLEX", zset_handler, 4, 4);
    add("ZRANGESTORE", zset_handler, 5, 0);
    add("ZRANK", zset_handler, 3, 4);
    add("ZREVRANK", zset_handler, 3, 4);
    add("ZPOPMIN", zset_handler, 2, 0);
    add("ZPOPMAX", zset_handler, 2, 0);
    add("ZMPOP", zset_handler, 4, 0);
    add("BZPOPMIN", zset_handler, 3, 0, redis_command_kind::blocking);
    add("BZPOPMAX", zset_handler, 3, 0, redis_command_kind::blocking);
    add("BZMPOP", zset_handler, 5, 0, redis_command_kind::blocking);
    add("ZUNION", zset_handler, 3, 0);
    add("ZINTER", zset_handler, 3, 0);
    add("ZDIFF", zset_handler, 3, 0);
    add("ZUNIONSTORE", zset_handler, 4, 0);
    add("ZINTERSTORE", zset_handler, 4, 0);
    add("ZDIFFSTORE", zset_handler, 4, 0);
    add("ZINTERCARD", zset_handler, 3, 0);
    add("DBSIZE", server_handler, 1, 1, redis_command_kind::dbsize);
    add("SELECT", server_handler, 2, 2);
    add("COPY", key_handler, 3, 0);
    add("MOVE", key_handler, 3, 3, redis_command_kind::move);
    add("PING", server_handler, 1, 2);
    add("ECHO", server_handler, 2, 2);
    add("QUIT", nullptr, 1, 1);
    add("RESET", nullptr, 1, 1);
    add("KEYS", scan_handler, 2, 2, redis_command_kind::keys);
    add("RANDOMKEY", scan_handler, 1, 1, redis_command_kind::randomkey);
    add("TIME", server_handler, 1, 1);
    add("SCAN", scan_handler, 2, 0);
    add("HSCAN", scan_handler, 3, 0);
    add("SSCAN", scan_handler, 3, 0);
    add("ZSCAN", scan_handler, 3, 0);
    add("INFO", server_handler, 1, 0, redis_command_kind::info);
    add("CONFIG", server_handler, 2, 0, redis_command_kind::config);
    add("COMMAND", server_handler, 1, 0);
    add("ACL", server_handler, 2, 0, redis_command_kind::acl);
    add("AUTH", server_handler, 2, 0);
    add("HELLO", server_handler, 1, 0);
    add("CLIENT", server_handler, 2, 0, redis_command_kind::client);
    add("CLUSTER", cluster_handler, 2, 0);
    add("INCR", string_handler, 2, 2);
    add("GETBIT", string_handler, 3, 3);
    add("SETBIT", string_handler, 4, 4);
    add("BITCOUNT", string_handler, 2, 0);
    add("BITPOS", string_handler, 3, 0);
    add("BITOP", string_handler, 4, 0);
    add("BITFIELD", string_handler, 2, 0);
    add("BITFIELD_RO", string_handler, 2, 0);
    add("DECR", string_handler, 2, 2);
    add("INCRBY", string_handler, 3, 3);
    add("INCRBYFLOAT", string_handler, 3, 3);
    add("DECRBY", string_handler, 3, 3);
    add("APPEND", string_handler, 3, 3);
    add("STRLEN", string_handler, 2, 2);
    add("GETRANGE", string_handler, 4, 4);
    add("SUBSTR", string_handler, 4, 4);
    add("SETRANGE", string_handler, 4, 4);
    add("GETDEL", string_handler, 2, 2);
    add("GETEX", string_handler, 2, 0);
    add("GETSET", string_handler, 3, 3);
    add("SETNX", string_handler, 3, 3);
    add("SETEX", string_handler, 4, 4);
    add("PSETEX", string_handler, 4, 4);
}

void redis_service::add(const char* name, redis_handler_t handler, size_t min,
    size_t max, redis_command_kind kind) {
    const auto* entry = command_catalog(name);
    const auto writes = entry && std::strstr(entry->categories, "@write");
    commands_.emplace(name, redis_command_spec{
        std::move(handler), name, min, max, statistics_.index(name), kind,
        writes});
}

std::string redis_service::acl_file(std::string path) {
    if (!path.empty()) {
        std::string text;
        auto error = acl_read_file(path, text);
        if (!error.empty()) return error;
        acl_reload reload;
        error = permissions_.prepare(text, reload);
        if (!error.empty()) return error;
        error = permissions_.commit(reload);
        if (!error.empty()) return error;
    }
    acl_file_.swap(path);
    return {};
}
const redis_command_spec* redis_service::get_command(const char* cmd) const {
    std::string name(cmd);
    std::transform(name.begin(), name.end(), name.begin(),
        [](unsigned char ch) {
            return static_cast<char>(ch >= 'a' && ch <= 'z' ?
                ch - ('a' - 'A') : ch);
        });
    auto it = commands_.find(name);
    return it == commands_.end() ? nullptr : &it->second;
}

std::vector<std::string> redis_service::command_names() const {
    std::vector<std::string> names;
    names.reserve(commands_.size());
    for (const auto& command : commands_) names.push_back(command.first);
    std::sort(names.begin(), names.end());
    return names;
}

bool redis_service::key_handler(redis_handler& handler, const char* cmd,
     const redis_object& obj, redis_coder& result) {
    redis_key redis(handler, obj);
    return redis.exec(cmd, result);
}

bool redis_service::geo_handler(redis_handler& handler, const char* command,
    const redis_object& object, redis_coder& result) {
    redis_geo value(handler, object);
    return value.exec(command, result);
}

bool redis_service::hll_handler(redis_handler& handler, const char* command,
    const redis_object& object, redis_coder& result) {
    redis_hyperloglog value(handler, object);
    return value.exec(command, result);
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

bool redis_service::zset_handler(redis_handler& handler, const char* cmd,
     const redis_object& obj, redis_coder& result) {
    redis_zset redis(handler, obj);
    return redis.exec(cmd, result);
}

bool redis_service::scan_handler(redis_handler& handler, const char* cmd,
     const redis_object& obj, redis_coder& result) {
    redis_scan redis(handler, obj);
    return redis.exec(cmd, result);
}

bool redis_service::set_handler(redis_handler& handler, const char* cmd,
     const redis_object& obj, redis_coder& result) {
    redis_set redis(handler, obj);
    return redis.exec(cmd, result);
}

bool redis_service::list_handler(redis_handler& handler, const char* cmd,
     const redis_object& obj, redis_coder& result) {
    redis_list redis(handler, obj);
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
