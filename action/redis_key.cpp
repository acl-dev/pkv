//
// Created by shuxin zheng on 2023/7/31.
//

#include "stdafx.h"
#include "common/redis_integer.h"
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
    { "RENAME",     true,   &redis_key::rename      },
    { "RENAMENX",   true,   &redis_key::rename      },
    { "DEL",        true,   &redis_key::del         },
    { "EXISTS",     true,   &redis_key::exists      },
    { "TYPE",       true,   &redis_key::type        },
    { "EXPIRE",     true,   &redis_key::expire      },
    { "PEXPIRE",    true,   &redis_key::expire      },
    { "EXPIREAT",   true,   &redis_key::expire      },
    { "PEXPIREAT",  true,   &redis_key::expire      },
    { "TTL",        true,   &redis_key::ttl         },
    { "PTTL",       true,   &redis_key::ttl         },
    { "EXPIRETIME", true,   &redis_key::ttl         },
    { "PEXPIRETIME", true,  &redis_key::ttl         },
    { "PERSIST",    true,   &redis_key::persist     },

    { nullptr,      false, nullptr            },
};

bool redis_key::exec(const char* cmd, redis_coder& result) {
    if (EQ(cmd, "COPY")) return copy(result);
    if (EQ(cmd, "MOVE")) return move(result);
    for (int i = 0; handlers[i].cmd != nullptr; i++) {
        if (EQ(cmd, handlers[i].cmd)) {
            if (check_cluster_mode(handlers[i].check_slot) && route_keys(cmd, result)) return true;
            return (this->*(handlers[i].func))(result);
        }
    }

    logger_error("Not support, cmd=%s", cmd);
    return false;
}

static bool key_error(dao::key& data, redis_coder& result) {
    result.create_object().set_error(data.error());
    return true;
}

bool redis_key::copy(redis_coder& result) {
    std::vector<std::string> keys{*obj_.argument(1), *obj_.argument(2)};
    if (check_cluster_mode(true) && route_keys(keys, result)) {
        return true;
    }
    const auto error = [&](const char* message) {
        result.create_object().set_error(message);
        return true;
    };
    const auto is = [](const std::string& option, const char* name) {
        return option.size() == std::strlen(name)
            && !strncasecmp(option.data(), name, option.size());
    };
    long long selected = static_cast<long long>(handler_.selected_database());
    bool replace = false;
    for (size_t i = 3; i < obj_.size(); ++i) {
        const auto& option = *obj_.argument(i);
        if (is(option, "REPLACE")) {
            replace = true;
        } else if (is(option, "DB") && i + 1 < obj_.size()) {
            if (!parse_redis_integer(*obj_.argument(++i), selected)) {
                return error("ERR value is not an integer or out of range");
            }
            if (selected < INT_MIN || selected > INT_MAX) {
                return error("ERR value is out of range, value must between -2147483648 and 2147483647");
            }
            if (selected < 0 || static_cast<unsigned long long>(selected) >= handler_.database_count()) {
                return error("ERR DB index is out of range");
            }
        } else {
            return error("ERR syntax error");
        }
    }
    if (var_cfg_cluster_mode && selected != 0) {
        return error("ERR Copying to another database is not allowed in cluster mode");
    }
    auto destination = handler_.database(static_cast<size_t>(selected));
    if (!destination) {
        return error("ERR storage operation failed");
    }
    bool copied;
    if (!dao_->copy(handler_.get_db(), keys[0], destination, keys[1], replace, copied)) {
        return key_error(*dao_, result);
    }
    result.create_object().set_number(copied ? 1 : 0);
    return true;
}

bool redis_key::rename(redis_coder& result) {
    const bool nx = EQ(obj_.get_cmd(), "RENAMENX");
    bool renamed;
    if (!dao_->rename(handler_.get_db(), *obj_.argument(1), *obj_.argument(2), nx, renamed)) return key_error(*dao_, result);
    if (nx) result.create_object().set_number(renamed ? 1 : 0);
    else result.create_object().set_status("OK");
    return true;
}

bool redis_key::del(redis_coder& result) {
    auto& db = handler_.get_db();
    long long removed;
    if (obj_.size() == 2) {
        if (!dao_->del_one(db, *obj_.argument(1), removed)) return key_error(*dao_, result);
    } else {
        std::vector<std::string> keys;
        keys.reserve(obj_.size() - 1);
        for (size_t i = 1; i < obj_.size(); ++i) keys.push_back(*obj_.argument(i));
        if (!dao_->del(db, keys, removed)) return key_error(*dao_, result);
    }
    result.create_object().set_number(removed);
    return true;
}

bool redis_key::exists(redis_coder& result) {
    std::vector<std::string> keys;
    for (size_t i = 1; i < obj_.size(); ++i) keys.push_back(*obj_.argument(i));
    long long count;
    if (!dao_->exists(handler_.get_db(), keys, count)) return key_error(*dao_, result);
    result.create_object().set_number(count);
    return true;
}

bool redis_key::type(redis_coder& result) {
    std::string type;
    if (dao_->type(handler_.get_db(), *obj_.argument(1), type)) {
        result.create_object().set_status(type);
    } else if (dao_->status() == dao::dao_base::read_status::missing) {
        result.create_object().set_status("none");
    } else return key_error(*dao_, result);
    return true;
}

bool redis_key::expire(redis_coder& result) {
    long long amount;
    if (!parse_redis_integer(*obj_.argument(2), amount)) {
        result.create_object().set_error("ERR value is not an integer or out of range");
        return true;
    }
    unsigned conditions = 0;
    for (size_t i = 3; i < obj_.size(); ++i) {
        const char* option = obj_[i];
        if (EQ(option, "NX")) conditions |= dao::key::expire_nx;
        else if (EQ(option, "XX")) conditions |= dao::key::expire_xx;
        else if (EQ(option, "GT")) conditions |= dao::key::expire_gt;
        else if (EQ(option, "LT")) conditions |= dao::key::expire_lt;
        else { result.create_object().set_error("ERR Unsupported option"); return true; }
    }
    if ((conditions & dao::key::expire_nx) && (conditions & ~dao::key::expire_nx)) {
        result.create_object().set_error("ERR NX and XX, GT or LT options at the same time are not compatible");
        return true;
    }
    if ((conditions & dao::key::expire_gt) && (conditions & dao::key::expire_lt)) {
        result.create_object().set_error("ERR GT and LT options at the same time are not compatible");
        return true;
    }
    const char* command = obj_.get_cmd();
    const bool milliseconds = EQ(command, "PEXPIRE") || EQ(command, "PEXPIREAT");
    const bool absolute = EQ(command, "EXPIREAT") || EQ(command, "PEXPIREAT");
    long long deadline;
    if (!expiration_deadline(amount, milliseconds ? 1 : 1000, absolute ? 0 : unix_time_ms(), deadline)) {
        std::string name(command);
        for (char& ch : name) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        result.create_object().set_error("ERR invalid expire time in '" + name + "' command");
        return true;
    }
    bool changed = dao_->set_expiry(handler_.get_db(), *obj_.argument(1), deadline, conditions);
    if (!changed && dao_->status() != dao::dao_base::read_status::missing &&
        dao_->status() != dao::dao_base::read_status::ok) return key_error(*dao_, result);
    result.create_object().set_number(changed ? 1 : 0);
    return true;
}

bool redis_key::ttl(redis_coder& result) {
    const char* command = obj_.get_cmd();
    const bool absolute = EQ(command, "EXPIRETIME") || EQ(command, "PEXPIRETIME");
    const bool milliseconds = EQ(command, "PTTL") || EQ(command, "PEXPIRETIME");
    long long value;
    const bool ok = absolute ? dao_->expiry_time(handler_.get_db(), *obj_.argument(1), value)
        : dao_->pttl(handler_.get_db(), *obj_.argument(1), value);
    if (ok) {
        if (!milliseconds && value >= 0) value = value / 1000 + (value % 1000 >= 500);
        result.create_object().set_number(value);
    } else if (dao_->status() == dao::dao_base::read_status::missing) {
        result.create_object().set_number(-2);
    } else return key_error(*dao_, result);
    return true;
}

bool redis_key::persist(redis_coder& result) {
    const bool changed = dao_->persist(handler_.get_db(), *obj_.argument(1));
    if (!changed && dao_->status() != dao::dao_base::read_status::missing &&
        dao_->status() != dao::dao_base::read_status::ok) return key_error(*dao_, result);
    result.create_object().set_number(changed ? 1 : 0);
    return true;
}

} // namespace pkv
