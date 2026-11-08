#pragma once
#include <cstddef>
#include <cassert>
#include <limits>
#include "replica_limits.h"

namespace pkv {

// Caller provides synchronization. Charges are encoded header/key/value bytes,
// not total allocator/RSS usage, and remain charged while a send is in flight.
class replica_budget {
public:
    replica_budget(size_t byte_limit = 64 * 1024 * 1024,
        size_t message_limit = 65536)
    : byte_limit_(byte_limit)
    , message_limit_(message_limit)
    , storage_limit_(storage_bound(byte_limit, message_limit))
    , action_limit_(scaled(message_limit, replica_action_factor))
    {}

    bool reserve(size_t key_size, size_t value_size) {
        const size_t available = byte_limit_ - bytes_;
        if (messages_ >= message_limit_ || available < header_size
            || key_size > available - header_size
            || value_size > available - header_size - key_size) {
            return false;
        }
        return reserve_encoded(header_size + key_size + value_size, 1);
    }

    void release(size_t key_size, size_t value_size) {
        assert(messages_ > 0 && bytes_ >= header_size
            && key_size <= bytes_ - header_size
            && value_size <= bytes_ - header_size - key_size);
        release_encoded(header_size + key_size + value_size, 1);
    }

    bool reserve_encoded(size_t bytes, size_t records, size_t storage = 0,
        size_t actions = 0) {
        if (!storage) storage = bytes;
        if (!actions) actions = records;
        if (!records || records > bytes / header_size ||
            records > message_limit_ - messages_ ||
            bytes > byte_limit_ - bytes_ ||
            storage > storage_limit_ - storage_ ||
            actions > action_limit_ - actions_) {
            return false;
        }
        bytes_ += bytes;
        messages_ += records;
        storage_ += storage; actions_ += actions;
        return true;
    }

    void release_encoded(size_t bytes, size_t records, size_t storage = 0,
        size_t actions = 0) {
        if (!storage) storage = bytes;
        if (!actions) actions = records;
        assert(records > 0 && records <= messages_ && bytes <= bytes_ &&
            storage <= storage_ && actions <= actions_);
        bytes_ -= bytes;
        messages_ -= records;
        storage_ -= storage; actions_ -= actions;
    }

    size_t bytes() const { return bytes_; }
    size_t messages() const { return messages_; }
    size_t expanded_bytes() const { return storage_; }
    size_t expanded_records() const { return actions_; }

private:
    static size_t scaled(size_t value, size_t factor) {
        const auto maximum = std::numeric_limits<size_t>::max();
        return value > maximum / factor ? maximum : value * factor;
    }
    static size_t storage_bound(size_t bytes, size_t records) {
        const auto payload = scaled(bytes, replica_storage_factor);
        const auto objects = scaled(scaled(records, replica_action_factor),
            replica_object_padding);
        const auto maximum = std::numeric_limits<size_t>::max();
        return objects > maximum - payload ? maximum : payload + objects;
    }
    static const size_t header_size = 20;
    size_t byte_limit_;
    size_t message_limit_;
    size_t storage_limit_, action_limit_;
    size_t bytes_ = 0;
    size_t messages_ = 0;
    size_t storage_ = 0, actions_ = 0;
};

} // namespace pkv
