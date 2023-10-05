#pragma once

#include <acl-lib/acl_cpp/stdlib/mbox.hpp>
#include "kv_message.h"

namespace pkv {

template<typename T>
class mbox {
public:
    mbox() = default;
    ~mbox() = default;
    void push(T* t) {
        box_.push(t);
    }

    T* pop(int timeout = -1) {
        return box_.pop(timeout);
    }

private:
    // In the current benchmark testing, acl::mbox is the most quickly.
#if defined(USE_TBOX)
    acl::tbox<T> box_;
#else
    acl::mbox<T> box_;
#endif
};

class message_box {
public:
    message_box() = default;
    ~message_box() = default;

    void push(kv_message* message) {
        box_.push(message);
    }

    kv_message* pop(int timeout) {
        return box_.pop(timeout);
    }

private:
    mbox<kv_message> box_;
};
} // namespace
