#pragma once
#include "common/kv_message.h"
# include <tbb/concurrent_queue.h>

namespace pkv {

class tbb_box {
public:
    tbb_box() = default;
    ~tbb_box() = default;

    void push(kv_message* message) {
        queue_.push(message);
    }

    kv_message* pop(int timeout = -1) {
        (void) timeout;
        kv_message* message;
        queue_.pop(message);
        return message;
    }

private:
    tbb::concurrent_bounded_queue<kv_message*> queue_;
};

} // namespace pkv
