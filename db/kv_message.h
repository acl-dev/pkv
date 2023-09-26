#include <utility>

//
// Created by shuxin zheng on 2023/9/26.
//
#pragma once

namespace pkv {

typedef enum {
    message_type_set,
    message_type_get,
    message_type_del,
} message_type;

class kv_message {
public:
    kv_message(std::string key, std::string value, message_type type)
    : key_(std::move(key)), value_(std::move(value)), type_(type) {}

    ~kv_message() = default;

    NODISCARD const std::string &get_key() const {
        return key_;
    }

    NODISCARD const std::string &get_value() const {
        return value_;
    }

    NODISCARD message_type get_type() const {
        return type_;
    }

    NODISCARD std::string get_type_s() const {
        switch (get_type()) {
        case message_type_set:
            return "set";
        case message_type_get:
            return "get";
        case message_type_del:
            return "del";
        default:
            return "unknown";
        }
    }

private:
    std::string key_;
    std::string value_;
    message_type type_;
};

using shared_message = std::shared_ptr<kv_message>;

} // namespace pkv
