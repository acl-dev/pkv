#pragma once
#include <acl-lib/fiber/fiber.hpp>
#include <acl-lib/fiber/fiber_cond.hpp>
#include <acl-lib/fiber/fiber_mutex.hpp>
#include <cstdlib>
#include <list>
#include <mutex>
#include <new>

namespace pkv {
// Multiple producers, one consumer. Failed push retains caller ownership.
// Closing wakes the consumer without allocating a sentinel list node.
template<class T> class fiber_mailbox {
public:
    bool push(T* item) {
        if (!item) return false;
        auto guard = lock();
        if (closed_) return false;
        try { items_.push_back(item); }
        catch (const std::bad_alloc&) {
            closed_ = true; ready_.notify(); return false;
        }
        ready_.notify();
        return true;
    }
    void close() {
        auto guard = lock();
        closed_ = true; ready_.notify();
    }
    T* pop(int timeout = -1, bool* closed = nullptr) {
        auto guard = lock();
        while (items_.empty() && !closed_) {
            if (!timeout) break;
            const bool notified = ready_.wait(mutex_, timeout);
            if (acl::fiber::self_killed()) closed_ = true;
            if (!notified && timeout >= 0) break;
        }
        if (closed) *closed = closed_;
        if (items_.empty()) return nullptr;
        auto item = items_.front(); items_.pop_front();
        return item;
    }
private:
    std::unique_lock<acl::fiber_mutex> lock() {
        if (!mutex_.lock()) std::abort();
        return std::unique_lock<acl::fiber_mutex>(mutex_, std::adopt_lock);
    }
    acl::fiber_mutex mutex_;
    acl::fiber_cond ready_;
    std::list<T*> items_;
    bool closed_ = false;
};
} // namespace pkv
