#include <utility>

//
// Created by shuxin zheng on 2023/9/26.
//
#pragma once

#define MESSAGE_KEY_MAX 65525
#define MESSAGE_VAL_MAX 1024000

namespace pkv {

typedef enum {
    message_oper_set = 1,
    message_oper_get = 2,
    message_oper_del = 3,
    message_oper_max = 4,
} message_oper_t;

class kv_message {
public:
    kv_message(std::string key, std::string value, message_oper_t oper)
    : key_(std::move(key)), value_(std::move(value)), oper_(oper) {}

    ~kv_message() = default;

    NODISCARD const std::string &get_key() const {
        return key_;
    }

    NODISCARD const std::string &get_value() const {
        return value_;
    }

    NODISCARD message_oper_t get_oper() const {
        return oper_;
    }

    NODISCARD std::string get_oper_s() const {
        switch (get_oper()) {
        case message_oper_set:
            return "set";
        case message_oper_get:
            return "get";
        case message_oper_del:
            return "del";
        default:
            return "unknown";
        }
    }

private:
    std::string key_;
    std::string value_;
    message_oper_t oper_;
};

using shared_message = std::shared_ptr<kv_message>;

} // namespace pkv
