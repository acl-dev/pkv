//
// Created by shuxin zheng on 2023/7/31.
//

#include "stdafx.h"
#include "common/redis_integer.h"
#include "dao/paged_string.h"

#include "redis_handler.h"
#include "redis_string.h"

namespace pkv {

#define EQ  !strcasecmp

redis_string::redis_string(redis_handler& handler, const redis_object &obj)
: redis_command(handler, obj)
{
    dao_ = new dao::paged_string;
}

redis_string::~redis_string() {
    delete dao_;
}

struct string_handler {
    const char* cmd;
    bool check_slot;
    bool (redis_string::*func) (redis_coder&);
};

static struct string_handler handlers[] = {
    { "GETBIT",     true,   &redis_string::bitmap },
    { "SETBIT",     true,   &redis_string::bitmap },
    { "BITCOUNT",   true,   &redis_string::bitcount },
    { "BITPOS",     true,   &redis_string::bitpos },
    { "BITOP",      true,   &redis_string::bitop },
    { "BITFIELD",   true,   &redis_string::bitfield },
    { "BITFIELD_RO", true,  &redis_string::bitfield },
    { "SET",        true,   &redis_string::set      },
    { "GET",        true,   &redis_string::get      },
    { "DIGEST",     true,   &redis_string::digest   },
    { "DELEX",      true,   &redis_string::delex    },
    { "INCR",       true,   &redis_string::increment },
    { "DECR",       true,   &redis_string::increment },
    { "INCRBY",     true,   &redis_string::increment },
    { "INCRBYFLOAT", true, &redis_string::increment_float },
    { "DECRBY",     true,   &redis_string::increment },
    { "APPEND",     true,   &redis_string::append    },
    { "STRLEN",     true,   &redis_string::length    },
    { "GETRANGE",   true,   &redis_string::getrange  },
    { "SUBSTR",     true,   &redis_string::getrange  },
    { "SETRANGE",   true,   &redis_string::setrange  },
    { "GETDEL",     true,   &redis_string::getex     },
    { "GETEX",      true,   &redis_string::getex     },
    { "GETSET",     true,   &redis_string::legacy_set },
    { "SETNX",      true,   &redis_string::legacy_set },
    { "SETEX",      true,   &redis_string::legacy_set },
    { "PSETEX",     true,   &redis_string::legacy_set },
    { "LCS",        true,   &redis_string::lcs      },
    { "MGET",       true,   &redis_string::mget     },
    { "MSET",       true,   &redis_string::mset     },
    { "MSETEX",     true,   &redis_string::msetex   },
    { "MSETNX",     true,   &redis_string::mset     },

    { nullptr,      false, nullptr            },
};

bool redis_string::exec(const char* cmd, redis_coder& result) {
    for (int i = 0; handlers[i].cmd != nullptr; i++) {
        if (EQ(cmd, handlers[i].cmd)) {
            if (check_cluster_mode(handlers[i].check_slot) && route_keys(cmd, result)) return true;
            return (this->*(handlers[i].func))(result);
        }
    }

    logger_error("Not support, cmd=%s", cmd);
    return false;
}

static bool set_option_is(const std::string& value, const char* expected) {
    return value.size() == std::strlen(expected) &&
        !strncasecmp(value.data(), expected, value.size());
}
static dao::set_options::condition comparison_option(const std::string& name) {
    using option = dao::set_options;
    if (set_option_is(name, "IFEQ")) return option::only_equal;
    if (set_option_is(name, "IFNE")) return option::only_different;
    if (set_option_is(name, "IFDEQ")) return option::digest_equal;
    if (set_option_is(name, "IFDNE")) return option::digest_different;
    return option::invalid_condition;
}
bool redis_string::set(redis_coder& result) {
    dao::set_options options;
    using condition = dao::set_options;
    enum expiry_mode { none, ex, px, exat, pxat, keep } mode = none;
    const std::string* expiry = nullptr;
    for (size_t i = 3; i < obj_.size(); ++i) {
        const auto& option = *obj_.argument(i);
        auto next_condition = comparison_option(option);
        if (next_condition == condition::invalid_condition)
            next_condition = condition::any;
        if (set_option_is(option, "NX"))
            next_condition = condition::only_missing;
        if (set_option_is(option, "XX"))
            next_condition = condition::only_existing;
        if (set_option_is(option, "IFEQ"))
            next_condition = condition::only_equal;
        if (set_option_is(option, "IFNE"))
            next_condition = condition::only_different;
        if (next_condition != condition::any) {
            if (options.when != condition::any &&
                options.when != next_condition) {
                result.create_object().set_error("ERR syntax error");
                return true;
            }
            options.when = next_condition;
            if (options.compares()) {
                if (++i == obj_.size()) {
                    result.create_object().set_error("ERR syntax error");
                    return true;
                }
                options.comparison = obj_.argument(i);
            }
        } else if (set_option_is(option, "GET")) {
            options.return_old = true;
        } else if (set_option_is(option, "KEEPTTL") &&
            (mode == none || mode == keep)) {
            options.keep_ttl = true;
            mode = keep;
        } else {
            expiry_mode next = none;
            if (set_option_is(option, "EX")) next = ex;
            else if (set_option_is(option, "PX")) next = px;
            else if (set_option_is(option, "EXAT")) next = exat;
            else if (set_option_is(option, "PXAT")) next = pxat;
            if (next == none || (mode != none && mode != next) ||
                i + 1 >= obj_.size()) {
                result.create_object().set_error("ERR syntax error");
                return true;
            }
            mode = next;
            expiry = obj_.argument(++i);
        }
    }
    if (expiry) {
        long long amount;
        if (!parse_redis_integer(*expiry, amount)) {
            result.create_object().set_error(
                "ERR value is not an integer or out of range");
            return true;
        }
        if (amount <= 0 || !expiration_deadline(amount,
            mode == ex || mode == exat ? 1000 : 1,
            mode == exat || mode == pxat ? 0 : unix_time_ms(),
            options.expires_at_ms)) {
            result.create_object().set_error(
                "ERR invalid expire time in 'set' command");
            return true;
        }
    }
    if (var_cfg_disable_save) {
        result.create_object().set_status("OK");
        return true;
    }
    dao::set_result outcome;
    if (!dao_->set(handler_.get_db(), *obj_.argument(1), *obj_.argument(2),
        options, outcome)) {
        result.create_object().set_error(dao_->error());
    } else if (options.return_old) {
        if (outcome.had_old)
            result.create_object().set_string(outcome.old_value);
        else result.create_object().set_null();
    } else if (!outcome.applied) {
        result.create_object().set_null();
    } else {
        result.create_object().set_status("OK");
    }
    return true;
}

bool redis_string::digest(redis_coder& result) {
    dao::string_result out;
    if (!dao_->digest(handler_.get_db(), *obj_.argument(1), out))
        result.create_object().set_error(dao_->error());
    else if (out.found) result.create_object().set_string(out.value);
    else result.create_object().set_null();
    return true;
}
bool redis_string::delex(redis_coder& result) {
    if (obj_.size() != 2 && obj_.size() != 4) {
        result.create_object().set_error(
            "ERR wrong number of arguments for 'delex' command");
        return true;
    }
    dao::set_options options;
    if (obj_.size() == 4) {
        options.when = comparison_option(*obj_.argument(2));
        options.comparison = obj_.argument(3);
    }
    long long removed;
    if (!dao_->delex(handler_.get_db(), *obj_.argument(1), options, removed))
        result.create_object().set_error(dao_->error());
    else result.create_object().set_number(removed);
    return true;
}

bool redis_string::mget(redis_coder& result) {
    std::vector<std::string> keys;
    for (size_t i = 1; i < obj_.size(); ++i) keys.push_back(*obj_.argument(i));
    std::vector<dao::string_result> values;
    if (!dao_->mget(handler_.get_db(), keys, values)) {
        result.create_object().set_error(dao_->error());
        return true;
    }
    auto& reply = result.create_object();
    reply.set_type(REDIS_OBJ_ARRAY);
    for (const auto& value : values) {
        if (value.found) reply.create_child().set_string(value.value);
        else reply.create_child().set_null();
    }
    return true;
}

bool redis_string::mset(redis_coder& result) {
    const bool nx = EQ(obj_.get_cmd(), "MSETNX");
    if (obj_.size() % 2 != 1) {
        result.create_object().set_error(nx ? "ERR wrong number of arguments for 'msetnx' command"
                                          : "ERR wrong number of arguments for 'mset' command");
        return true;
    }
    std::map<std::string, std::string> values;
    for (size_t i = 1; i < obj_.size(); i += 2) values[*obj_.argument(i)] = *obj_.argument(i + 1);
    bool applied;
    if (!dao_->mset(handler_.get_db(), values, nx, applied)) result.create_object().set_error(dao_->error());
    else if (nx) result.create_object().set_number(applied ? 1 : 0);
    else result.create_object().set_status("OK");
    return true;
}

bool redis_string::increment(redis_coder& result) {
    const char* command = obj_.get_cmd();
    long long delta = EQ(command, "DECR") ? -1 : 1;
    if (obj_.size() == 3 && !parse_redis_integer(*obj_.argument(2), delta)) {
        result.create_object().set_error("ERR value is not an integer or out of range");
        return true;
    }
    long long value;
    if (!dao_->increment(handler_.get_db(), *obj_.argument(1), delta, EQ(command, "DECRBY"), value))
        result.create_object().set_error(dao_->error());
    else result.create_object().set_number(value);
    return true;
}

bool redis_string::append(redis_coder& result) {
    long long length;
    if (!dao_->append(handler_.get_db(), *obj_.argument(1), *obj_.argument(2), length))
        result.create_object().set_error(dao_->error());
    else result.create_object().set_number(length);
    return true;
}

bool redis_string::increment_float(redis_coder& result) {
    std::string value;
    if (!dao_->increment_float(handler_.get_db(), *obj_.argument(1),
        *obj_.argument(2), value)) {
        result.create_object().set_error(dao_->error()); return true;
    }
    result.create_object().set_string(value);
    return true;
}

bool redis_string::setrange(redis_coder& result) {
    long long offset;
    if (!parse_redis_integer(*obj_.argument(2), offset)) {
        result.create_object().set_error("ERR value is not an integer or out of range");
        return true;
    }
    long long length;
    if (!dao_->setrange(handler_.get_db(), *obj_.argument(1), offset, *obj_.argument(3), length))
        result.create_object().set_error(dao_->error());
    else result.create_object().set_number(length);
    return true;
}

bool redis_string::length(redis_coder& result) {
    long long length;
    if (!dao_->length(handler_.get_db(), *obj_.argument(1), length)) result.create_object().set_error(dao_->error());
    else result.create_object().set_number(length);
    return true;
}

bool redis_string::getrange(redis_coder& result) {
    long long start, end;
    if (!parse_redis_integer(*obj_.argument(2), start) || !parse_redis_integer(*obj_.argument(3), end)) {
        result.create_object().set_error("ERR value is not an integer or out of range");
        return true;
    }
    std::string value;
    if (!dao_->getrange(handler_.get_db(), *obj_.argument(1), start, end, value)) result.create_object().set_error(dao_->error());
    else result.create_object().set_string(value);
    return true;
}

bool redis_string::getex(redis_coder& result) {
    dao::get_options options;
    if (EQ(obj_.get_cmd(), "GETDEL")) options.expiry = dao::get_options::erase;
    else for (size_t i = 2; i < obj_.size(); ++i) {
        const char* option = obj_[i];
        auto next = dao::get_options::unchanged;
        if (EQ(option, "PERSIST")) next = dao::get_options::persist;
        else if (EQ(option, "EX")) next = dao::get_options::ex;
        else if (EQ(option, "PX")) next = dao::get_options::px;
        else if (EQ(option, "EXAT")) next = dao::get_options::exat;
        else if (EQ(option, "PXAT")) next = dao::get_options::pxat;
        if (next == dao::get_options::unchanged ||
            (options.expiry != dao::get_options::unchanged && options.expiry != next) ||
            (next != dao::get_options::persist && i + 1 >= obj_.size())) {
            result.create_object().set_error("ERR syntax error");
            return true;
        }
        options.expiry = next;
        if (next != dao::get_options::persist) options.amount = *obj_.argument(++i);
    }
    dao::string_result value;
    if (!dao_->getex(handler_.get_db(), *obj_.argument(1), options, value)) result.create_object().set_error(dao_->error());
    else if (value.found) result.create_object().set_string(value.value);
    else result.create_object().set_null();
    return true;
}

bool redis_string::legacy_set(redis_coder& result) {
    const char* command = obj_.get_cmd();
    dao::set_options options;
    options.when = EQ(command, "SETNX") ? dao::set_options::only_missing : dao::set_options::any;
    options.return_old = EQ(command, "GETSET");
    size_t value_index = 2;
    if (EQ(command, "SETEX") || EQ(command, "PSETEX")) {
        value_index = 3;
        long long amount;
        if (!parse_redis_integer(*obj_.argument(2), amount)) {
            result.create_object().set_error("ERR value is not an integer or out of range");
            return true;
        }
        if (amount <= 0 || !expiration_deadline(amount, EQ(command, "SETEX") ? 1000 : 1,
                                               unix_time_ms(), options.expires_at_ms)) {
            result.create_object().set_error(EQ(command, "SETEX") ? "ERR invalid expire time in 'setex' command"
                                                                : "ERR invalid expire time in 'psetex' command");
            return true;
        }
    }
    dao::set_result outcome;
    if (!dao_->set(handler_.get_db(), *obj_.argument(1), *obj_.argument(value_index), options, outcome))
        result.create_object().set_error(dao_->error());
    else if (options.return_old) {
        if (outcome.had_old) result.create_object().set_string(outcome.old_value);
        else result.create_object().set_null();
    } else if (EQ(command, "SETNX")) result.create_object().set_number(outcome.applied ? 1 : 0);
    else result.create_object().set_status("OK");
    return true;
}

bool redis_string::get(redis_coder& result) {
    const auto* key = obj_.argument(1);
    if (!key) {
        result.create_object().set_error("ERR invalid arguments");
        return true;
    }
    std::string value;
    if (dao_->get(handler_.get_db(), *key, value)) {
        result.create_object().set_string(value);
    } else if (dao_->status() == dao::dao_base::read_status::missing) {
        result.create_object().set_null();
    } else {
        result.create_object().set_error(dao_->error());
    }
    return true;
}

} // namespace pkv
