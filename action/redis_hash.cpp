//
// Created by shuxin zheng on 2023/8/1.
//

#include "stdafx.h"
#include "dao/paged_hash.h"

#include "redis_handler.h"
#include "redis_hash.h"
#include "common/redis_integer.h"

namespace pkv {

#define EQ  !strcasecmp

redis_hash::redis_hash(redis_handler &handler, const redis_object &obj)
: redis_command(handler, obj)
{
    dao_ = new dao::paged_hash;
}

redis_hash::~redis_hash() {
    delete dao_;
}

struct hash_handler {
    const char* cmd;
    bool check_slot;
    bool (redis_hash::*func) (redis_coder&);
};

static struct hash_handler handlers[] = {
    { "HSET",       true,   &redis_hash::hset       },
    { "HGET",       true,   &redis_hash::hget       },
    { "HGETDEL",    true,   &redis_hash::hgetdel    },
    { "HDEL",       true,   &redis_hash::hdel       },
    { "HMSET",      true,   &redis_hash::hmset      },
    { "HMGET",      true,   &redis_hash::hmget      },
    { "HGETALL",    true,   &redis_hash::hgetall    },
    { "HSETNX", true, &redis_hash::hsetnx },
    { "HINCRBY", true, &redis_hash::hincrby },
    { "HINCRBYFLOAT", true, &redis_hash::hincrbyfloat },
    { "HLEN", true, &redis_hash::hlen },
    { "HSTRLEN", true, &redis_hash::hstrlen },
    { "HEXISTS", true, &redis_hash::hexists },
    { "HKEYS", true, &redis_hash::hkeys },
    { "HVALS", true, &redis_hash::hvals },
    { "HRANDFIELD", true, &redis_hash::hrandfield },
    { nullptr,      false, nullptr            },
};

bool redis_hash::exec(const char* cmd, redis_coder& result) {
    for (int i = 0; handlers[i].cmd != nullptr; i++) {
        if (EQ(cmd, handlers[i].cmd)) {
            if (check_cluster_mode(handlers[i].check_slot) && route_keys(cmd, result)) return true;
            return (this->*(handlers[i].func))(result);
        }
    }

    logger_error("Not support, cmd=%s", cmd);
    return false;
}

static bool hash_error(dao::hash& data, redis_coder& result) {
    result.create_object().set_error(data.error());
    return true;
}

bool redis_hash::hset(redis_coder& result) {
    if (obj_.size() % 2 != 0) {
        result.create_object().set_error("ERR wrong number of arguments for 'hset' command");
        return true;
    }
    std::map<std::string, std::string> fields;
    for (size_t i = 2; i < obj_.size(); i += 2) fields[*obj_.argument(i)] = *obj_.argument(i + 1);
    int added = dao_->hmset(handler_.get_db(), *obj_.argument(1), fields);
    if (added < 0) return hash_error(*dao_, result);
    result.create_object().set_number(added);
    return true;
}

bool redis_hash::hget(redis_coder& result) {
    std::string value;
    if (dao_->hget(handler_.get_db(), *obj_.argument(1), *obj_.argument(2), value)) {
        result.create_object().set_string(value);
    } else if (dao_->status() == dao::dao_base::read_status::missing) {
        result.create_object().set_null();
    } else return hash_error(*dao_, result);
    return true;
}

bool redis_hash::hdel(redis_coder& result) {
    std::vector<std::string> names;
    for (size_t i = 2; i < obj_.size(); ++i) names.push_back(*obj_.argument(i));
    int removed = dao_->hdel(handler_.get_db(), *obj_.argument(1), names);
    if (removed < 0) return hash_error(*dao_, result);
    result.create_object().set_number(removed);
    return true;
}

bool redis_hash::hmset(redis_coder& result) {
    if (obj_.size() % 2 != 0) {
        result.create_object().set_error("ERR wrong number of arguments for 'hmset' command");
        return true;
    }
    std::map<std::string, std::string> fields;
    for (size_t i = 2; i < obj_.size(); i += 2) fields[*obj_.argument(i)] = *obj_.argument(i + 1);
    if (dao_->hmset(handler_.get_db(), *obj_.argument(1), fields) < 0) return hash_error(*dao_, result);
    result.create_object().set_status("OK");
    return true;
}

bool redis_hash::hmget(redis_coder& result) {
    std::vector<std::string> names;
    for (size_t i = 2; i < obj_.size(); ++i) names.push_back(*obj_.argument(i));
    std::map<std::string, std::string> fields;
    if (!dao_->hmget(handler_.get_db(), *obj_.argument(1), names, fields)) return hash_error(*dao_, result);
    auto& reply = result.create_object();
    reply.set_type(REDIS_OBJ_ARRAY);
    for (size_t i = 2; i < obj_.size(); ++i) {
        auto it = fields.find(*obj_.argument(i));
        if (it == fields.end()) reply.create_child().set_null();
        else reply.create_child().set_string(it->second);
    }
    return true;
}

bool redis_hash::hgetall(redis_coder& result) {
    if (!dao_->hgetall(handler_.get_db(), *obj_.argument(1))) return hash_error(*dao_, result);
    auto& reply = result.create_object();
    reply.set_type(REDIS_OBJ_ARRAY);
    reply.response_as(redis_object::response_kind::map);
    for (const auto& field : dao_->get_fields()) {
        reply.create_child().set_string(field.first);
        reply.create_child().set_string(field.second);
    }
    return true;
}

bool redis_hash::hsetnx(redis_coder& result) {
    bool added;
    if (!dao_->hsetnx(handler_.get_db(), *obj_.argument(1), *obj_.argument(2), *obj_.argument(3), added))
        return hash_error(*dao_, result);
    result.create_object().set_number(added ? 1 : 0);
    return true;
}

bool redis_hash::hincrby(redis_coder& result) {
    long long delta, value;
    if (!parse_redis_integer(*obj_.argument(3), delta)) {
        result.create_object().set_error("ERR value is not an integer or out of range");
        return true;
    }
    if (!dao_->hincrby(handler_.get_db(), *obj_.argument(1), *obj_.argument(2), delta, value))
        return hash_error(*dao_, result);
    result.create_object().set_number(value);
    return true;
}

bool redis_hash::hlen(redis_coder& result) {
    long long length;
    if (!dao_->hlen(handler_.get_db(), *obj_.argument(1), length)) return hash_error(*dao_, result);
    result.create_object().set_number(length);
    return true;
}

bool redis_hash::hincrbyfloat(redis_coder& result) {
    std::string value;
    if (!dao_->hincrbyfloat(handler_.get_db(), *obj_.argument(1),
        *obj_.argument(2), *obj_.argument(3), value))
        return hash_error(*dao_, result);
    result.create_object().set_string(value);
    return true;
}

bool redis_hash::hstrlen(redis_coder& result) {
    long long length;
    bool exists;
    if (!dao_->hstrlen(handler_.get_db(), *obj_.argument(1), *obj_.argument(2), length, exists)) return hash_error(*dao_, result);
    result.create_object().set_number(length);
    return true;
}

bool redis_hash::hexists(redis_coder& result) {
    long long length;
    bool exists;
    if (!dao_->hstrlen(handler_.get_db(), *obj_.argument(1), *obj_.argument(2), length, exists)) return hash_error(*dao_, result);
    result.create_object().set_number(exists ? 1 : 0);
    return true;
}

bool redis_hash::hkeys(redis_coder& result) {
    if (!dao_->hgetall(handler_.get_db(), *obj_.argument(1), false))
        return hash_error(*dao_, result);
    auto& reply = result.create_object();
    reply.set_type(REDIS_OBJ_ARRAY);
    for (const auto& field : dao_->get_fields())
        reply.create_child().set_string(field.first);
    return true;
}

bool redis_hash::hvals(redis_coder& result) {
    if (!dao_->hgetall(handler_.get_db(), *obj_.argument(1))) return hash_error(*dao_, result);
    auto& reply = result.create_object();
    reply.set_type(REDIS_OBJ_ARRAY);
    for (const auto& field : dao_->get_fields()) reply.create_child().set_string(field.second);
    return true;
}

bool redis_hash::hrandfield(redis_coder& result) {
    const auto error = [&](const char* message) {
        result.create_object().set_error(message); return true;
    };
    const bool has_count = obj_.size() >= 3;
    long long count = 1;
    if (has_count && !parse_redis_integer(*obj_.argument(2), count))
        return error("ERR value is not an integer or out of range");
    if (count == LLONG_MIN)
        return error("ERR value is out of range, value must between "
            "-9223372036854775807 and 9223372036854775807");
    if (obj_.size() > 4 || (obj_.size() == 4 &&
        strcasecmp(obj_.argument(3)->c_str(), "WITHVALUES")))
        return error("ERR syntax error");
    const bool values = obj_.size() == 4;
    if (values && (count < -LLONG_MAX / 2 || count > LLONG_MAX / 2))
        return error("ERR value is out of range");
    dao::hash::sample_values out;
    if (!dao_->sample(handler_.get_db(), *obj_.argument(1), count, values, out))
        return error(dao_->error());
    auto& reply = result.create_object();
    if (!has_count) {
        if (out.empty()) reply.set_null();
        else reply.set_string(out.front().first);
    } else {
        reply.set_type(REDIS_OBJ_ARRAY);
        if (values) reply.response_as(redis_object::response_kind::pairs);
        for (const auto& field : out) {
            reply.create_child().set_string(field.first);
            if (values) reply.create_child().set_string(field.second);
        }
    }
    return true;
}

} // namespace pkv
