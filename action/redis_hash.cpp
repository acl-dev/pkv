//
// Created by shuxin zheng on 2023/8/1.
//

#include "stdafx.h"
#include "dao/paged_hash.h"

#include "redis_handler.h"
#include "redis_hash.h"
#include "common/redis_integer.h"
#include <unordered_map>

namespace pkv {

#define EQ  !strcasecmp

redis_hash::redis_hash(redis_handler &handler, const redis_object &obj)
: redis_command(handler, obj), data_(handler.page_pipeline_scope_), dao_(&data_)
{}

redis_hash::~redis_hash() = default;

struct hash_handler {
    const char* cmd;
    bool check_slot;
    bool (redis_hash::*func) (redis_coder&);
};

static const struct hash_handler handlers[] = {
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
            return dispatch(cmd, handlers[i].check_slot, handlers[i].func,
                result);
        }
    }

    logger_error("Not support, cmd=%s", cmd);
    return false;
}

bool redis_hash::dispatch(const char* cmd, bool check_slot,
    bool (redis_hash::*operation)(redis_coder&), redis_coder& result) {
    if (check_cluster_mode(check_slot) && route_keys(cmd, result)) return true;
    return (this->*operation)(result);
}

redis_handler_t redis_hash::bind(const char* name) {
    for (const auto* entry = handlers; entry->cmd; ++entry) {
        if (strcasecmp(name, entry->cmd)) continue;
        return [entry](redis_handler& handler, const char* cmd,
            const redis_object& object, redis_coder& result) {
            redis_hash command(handler, object);
            return command.dispatch(cmd, entry->check_slot, entry->func,
                result);
        };
    }
    return {};
}

static bool hash_error(dao::hash& data, redis_coder& result) {
    result.create_object().set_error(data.error());
    return true;
}

static int hash_write_fields(dao::paged_hash& data, shared_db& db,
    const redis_object& object) {
    const auto& key = *object.argument(1);
    if (object.size() == 6)
        return data.hmset_pair(db, key, {object.argument(2), object.argument(3)},
            {object.argument(4), object.argument(5)});
    map_store::hash_field_refs fields;
    fields.reserve((object.size() - 2) / 2);
    for (size_t i = 2; i < object.size(); i += 2)
        fields.emplace_back(object.argument(i), object.argument(i + 1));
    return data.hmset(db, key, std::move(fields));
}

bool redis_hash::hset(redis_coder& result) {
    if (obj_.size() % 2 != 0) {
        result.create_object().set_error("ERR wrong number of arguments for 'hset' command");
        return true;
    }
    if (obj_.size() == 4) {
        bool added;
        if (!data_.hset(handler_.get_db(), *obj_.argument(1),
            *obj_.argument(2), *obj_.argument(3), added))
            return hash_error(data_, result);
        result.create_object().set_number(added ? 1 : 0);
        return true;
    }
    int added = hash_write_fields(data_, handler_.get_db(), obj_);
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
    if (obj_.size() == 4) {
        if (!data_.hset(handler_.get_db(), *obj_.argument(1),
            *obj_.argument(2), *obj_.argument(3)))
            return hash_error(data_, result);
        result.create_object().set_status("OK");
        return true;
    }
    if (hash_write_fields(data_, handler_.get_db(), obj_) < 0)
        return hash_error(*dao_, result);
    result.create_object().set_status("OK");
    return true;
}

static void append_hash_bulk(std::string& frame, const std::string& value) {
    frame += '$';
    frame += std::to_string(value.size());
    frame += "\r\n";
    frame += value;
    frame += "\r\n";
}

bool redis_hash::hmget(redis_coder& result) {
    constexpr auto limit = db::page_lock_scope::max_hash_batch;
    const auto count = obj_.size() - 2;
    if (count <= limit) {
        const std::string* fields[limit];
        std::string values[limit];
        for (size_t i = 0; i < count; ++i) fields[i] = obj_.argument(i + 2);
        unsigned found;
        if (!data_.hmget(handler_.get_db(), *obj_.argument(1), fields, count,
            values, found)) return hash_error(*dao_, result);
        auto& reply = result.create_object();
        reply.set_type(REDIS_OBJ_ARRAY);
        for (size_t i = 0; i < count; ++i) {
            auto& child = reply.create_child();
            if (found & (1U << i)) child.set_string(values[i]);
            else child.set_null();
        }
        return true;
    }
    std::string frame = "*" + std::to_string(count) + "\r\n";
    const std::string nil = handler_.client_.protocol() == 3 ?
        "_\r\n" : "$-1\r\n";
    const auto hash = [](const std::string* field) {
        return std::hash<std::string>{}(*field);
    };
    const auto equal = [](const std::string* a, const std::string* b) {
        return *a == *b;
    };
    using range = std::pair<size_t, size_t>;
    // Offsets survive frame growth and let duplicate fields reuse wire bytes.
    std::unordered_map<const std::string*, range, decltype(hash),
        decltype(equal)> encoded(0, hash, equal);
    if (count >= 128) {
        for (size_t i = 2; i < obj_.size(); ++i)
            encoded.emplace(obj_.argument(i), range{});
    }
    std::string scanned_bytes;
    bool scan_complete = false;
    const auto selected = [&](const std::string& field) {
        return scan_complete && encoded.find(&field) != encoded.end();
    };
    const auto collect = [&](std::string&& field, std::string&& value) {
        // Keep the temporary scan buffer bounded even for very large values.
        constexpr size_t staging_limit = 256 * 1024;
        constexpr size_t max_bulk_overhead = 32;
        if (value.size() > staging_limit - scanned_bytes.size() ||
            max_bulk_overhead > staging_limit - scanned_bytes.size() - value.size()) {
            scan_complete = false;
            std::string().swap(scanned_bytes);
            for (auto& item : encoded) item.second = range{};
            return true;
        }
        auto& bytes = encoded.find(&field)->second;
        if (bytes.second) return false;
        bytes.first = scanned_bytes.size();
        append_hash_bulk(scanned_bytes, value);
        bytes.second = scanned_bytes.size() - bytes.first;
        return true;
    };
    const auto append = [&](const dao::paged_hash::field_reader& read) {
        std::string value;
        for (size_t i = 2; i < obj_.size(); ++i) {
            const auto* field = obj_.argument(i);
            auto& bytes = encoded.emplace(field, range{}).first->second;
            if (bytes.second) {
                frame.append(frame, bytes.first, bytes.second);
                continue;
            }
            bytes.first = frame.size();
            bool found;
            if (!read(*field, value, found)) return false;
            if (found) append_hash_bulk(frame, value);
            else frame += nil;
            bytes.second = frame.size() - bytes.first;
        }
        return true;
    };
    if (!data_.read_fields(handler_.get_db(), *obj_.argument(1),
        std::cref(append), encoded.size(), std::cref(selected),
        std::cref(collect), scan_complete)) return hash_error(data_, result);
    if (scan_complete) {
        size_t size = frame.size();
        for (size_t i = 2; i < obj_.size(); ++i) {
            const auto length = encoded.find(obj_.argument(i))->second.second;
            const auto bytes = length ? length : nil.size();
            if (bytes > frame.max_size() - size)
                throw std::length_error("HMGET reply too large");
            size += bytes;
        }
        frame.reserve(size);
        for (size_t i = 2; i < obj_.size(); ++i) {
            const auto& bytes = encoded.find(obj_.argument(i))->second;
            if (bytes.second) frame.append(scanned_bytes, bytes.first, bytes.second);
            else frame += nil;
        }
    }
    result.create_object().set_raw(std::move(frame));
    return true;
}

enum class hash_reply_fields { keys, values, pairs };

static void hash_collection_reply(redis_object& reply,
    const dao::paged_hash::field_list& fields, hash_reply_fields mode,
    unsigned protocol = 2) {
    const bool pairs = mode == hash_reply_fields::pairs;
    const bool keys = mode != hash_reply_fields::values;
    const bool values = mode != hash_reply_fields::keys;
    if (!fields.empty()) {
        const bool map = pairs && protocol == 3;
        std::string frame(1, map ? '%' : '*');
        frame += std::to_string(fields.size() * (pairs && !map ? 2 : 1));
        frame += "\r\n";
        auto size = frame.size();
        const auto measure = [&](const std::string& value) {
            size_t overhead = 6;
            for (auto length = value.size(); length >= 10; length /= 10)
                ++overhead;
            if (overhead > frame.max_size() - size ||
                value.size() > frame.max_size() - size - overhead)
                throw std::length_error("hash collection reply too large");
            size += value.size() + overhead;
        };
        for (const auto& field : fields) {
            if (keys) measure(field.first);
            if (values) measure(field.second);
        }
        frame.reserve(size);
        for (const auto& field : fields) {
            if (keys) append_hash_bulk(frame, field.first);
            if (values) append_hash_bulk(frame, field.second);
        }
        reply.set_raw(std::move(frame));
        return;
    }
    reply.set_type(REDIS_OBJ_ARRAY);
    if (pairs) reply.response_as(redis_object::response_kind::map);
}

bool redis_hash::hgetall(redis_coder& result) {
    dao::paged_hash::field_list fields;
    if (!data_.hgetall(handler_.get_db(), *obj_.argument(1), true, fields))
        return hash_error(*dao_, result);
    hash_collection_reply(result.create_object(), fields, hash_reply_fields::pairs,
        handler_.client_.protocol());
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
    dao::paged_hash::field_list fields;
    if (!data_.hgetall(handler_.get_db(), *obj_.argument(1), false, fields))
        return hash_error(*dao_, result);
    hash_collection_reply(result.create_object(), fields, hash_reply_fields::keys);
    return true;
}

bool redis_hash::hvals(redis_coder& result) {
    dao::paged_hash::field_list fields;
    if (!data_.hgetall(handler_.get_db(), *obj_.argument(1), true, fields))
        return hash_error(*dao_, result);
    hash_collection_reply(result.create_object(), fields, hash_reply_fields::values);
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
