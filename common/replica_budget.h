#pragma once
#include <cstddef>
#include <cassert>

namespace pkv {

// Caller provides synchronization. Charges are encoded header/key/value bytes,
// not total allocator/RSS usage, and remain charged while a send is in flight.
class replica_budget {
public:
    replica_budget(size_t byte_limit = 64 * 1024 * 1024,
        size_t message_limit = 65536)
    : byte_limit_(byte_limit)
    , message_limit_(message_limit)
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

    bool reserve_encoded(size_t bytes, size_t records) {
        if (!records || records > bytes / header_size ||
            records > message_limit_ - messages_ ||
            bytes > byte_limit_ - bytes_) {
            return false;
        }
        bytes_ += bytes;
        messages_ += records;
        return true;
    }

    void release_encoded(size_t bytes, size_t records) {
        assert(records > 0 && records <= messages_ && bytes <= bytes_);
        bytes_ -= bytes;
        messages_ -= records;
    }

    size_t bytes() const { return bytes_; }
    size_t messages() const { return messages_; }

private:
    static const size_t header_size = 20;
    size_t byte_limit_;
    size_t message_limit_;
    size_t bytes_ = 0;
    size_t messages_ = 0;
};

} // namespace pkv
