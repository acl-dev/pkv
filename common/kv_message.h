#include <utility>
#include <atomic>
#include <memory>
#include <string>
#include "c++_patch.h"

//
// Created by shuxin zheng on 2023/9/26.
//
#pragma once

#define MESSAGE_KEY_MAX 65525
#define MESSAGE_VAL_MAX 1024000

namespace pkv {

class page_batch;
class exec_batch;

typedef enum {
    message_oper_set = 1,
    message_oper_get = 2,
    message_oper_del = 3,
    message_oper_ready = 4,
    message_oper_max = 5,
    message_oper_exec = 16, // Internal whole-EXEC queue unit.
    message_oper_pages = 12, // Internal queue unit, not a wire operation.
} message_oper_t;

class kv_message {
public:
    kv_message(std::string key, std::string value, message_oper_t oper,
        unsigned database = 0, unsigned protocol = 1)
    : key_(std::move(key)), value_(std::move(value)), oper_(oper)
    , database_(database), protocol_(protocol) {
        refers_ = 1;
    }

    explicit kv_message(std::shared_ptr<const page_batch> pages);
    explicit kv_message(std::shared_ptr<const exec_batch> group);
    const std::shared_ptr<const exec_batch>& get_exec() const {
        return exec_;
    }
    const std::shared_ptr<const page_batch>& get_pages() const {
        return pages_;
    }
    unsigned get_protocol() const { return protocol_; }
    size_t expanded_bytes() const;
    size_t expanded_records() const;
    size_t wire_bytes() const;
    size_t wire_records() const;

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

    unsigned get_database() const {
        return database_;
    }

    void refer(size_t n) {
        refers_ += n;
    }

    void unrefer() {
        if (--refers_ == 0) {
            delete this;
        }
    }

private:
    std::string key_;
    std::string value_;
    message_oper_t oper_;
    unsigned database_;
    unsigned protocol_;
    std::shared_ptr<const page_batch> pages_;
    std::shared_ptr<const exec_batch> exec_;

    std::atomic<int> refers_;

public:
    ~kv_message() = default;
};

using shared_message = std::shared_ptr<kv_message>;

} // namespace pkv
