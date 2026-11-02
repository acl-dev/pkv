#include "stdafx.h"
#include "execution_gate.h"
#include <atomic>
#include <chrono>
#include <mutex>
#include <set>
#include <stdexcept>

namespace pkv {
namespace {
struct admission {
    size_t shared = 0;
    size_t exclusive = 0;
    size_t replica_reads = 0;
    long long time = 0, microseconds = 0;
};
long long wall_time_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}
long long wall_time_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}
void free_admission(void* value) { delete static_cast<admission*>(value); }
admission& current() {
    if (!acl_fiber_running()) {
        static thread_local admission thread;
        return thread;
    }
    // ACL's specific-key namespace belongs to each thread's scheduler.
    static thread_local int key = 0;
    auto* value = key > 0 ?
        static_cast<admission*>(acl_fiber_get_specific(key)) : nullptr;
    if (!value) {
        std::unique_ptr<admission> created(new admission);
        if (acl_fiber_set_specific(&key, created.get(), free_admission) < 0)
            throw std::runtime_error("cannot set execution fiber state");
        value = created.release();
    }
    return *value;
}
struct reader_slot {
    std::atomic<size_t> readers{0};
    bool registered = false;
    ~reader_slot();
};
reader_slot& local_slot();

class gate {
public:
    void add(reader_slot& slot) {
        auto lock = locked();
        slots_.insert(&slot); slot.registered = true;
    }
    void remove(reader_slot& slot) {
        auto lock = locked();
        if (slot.readers.load()) std::terminate();
        slots_.erase(&slot);
    }
    void enter_shared() {
        auto& local = current();
        if (local.shared || local.exclusive) { ++local.shared; return; }
        auto& slot = local_slot();
        for (;;) {
            // Seq-cst orders admission against close-and-drain. Different
            // acquire/release atomics could allow the reader/writer to miss.
            if (!closed_.load()) {
                slot.readers.fetch_add(1);
                if (!closed_.load()) { local.shared = 1; return; }
                leave_reader(slot);
            }
            auto lock = locked();
            ++waiting_readers_;
            while (closed_.load()) readers_ready_.wait(mutex_);
            --waiting_readers_;
            if (waiting_readers_) readers_ready_.notify();
        }
    }
    void leave_shared() {
        auto& local = current();
        assert(local.shared);
        if (--local.shared || local.exclusive) return;
        leave_reader(local_slot());
    }
    size_t pause() {
        auto& local = current();
        if (local.exclusive || !local.shared)
            throw std::logic_error("only shared admission can wait");
        const auto depth = local.shared;
        local.shared = 1;
        leave_shared();
        return depth;
    }
    void resume(size_t depth) {
        const auto& local = current();
        if (!depth || local.shared || local.exclusive) std::terminate();
        enter_shared();
        current().shared = depth;
    }
    void enter_exclusive() {
        auto& local = current();
        if (local.exclusive) { ++local.exclusive; return; }
        if (local.shared)
            throw std::logic_error("execution gate cannot upgrade a reader");
        auto lock = locked();
        ++waiting_writers_;
        closed_.store(true);
        while (writer_ || readers_active()) writers_ready_.wait(mutex_);
        --waiting_writers_;
        writer_ = true; local.exclusive = 1;
        local.microseconds = wall_time_us();
        local.time = local.microseconds / 1000;
    }
    void leave_exclusive() {
        auto& local = current();
        assert(local.exclusive);
        if (--local.exclusive) return;
        assert(!local.shared);
        auto lock = locked();
        writer_ = false;
        if (waiting_writers_) writers_ready_.notify();
        else {
            closed_.store(false);
            if (waiting_readers_) readers_ready_.notify();
        }
    }
private:
    std::atomic<bool> closed_{false};
    acl::fiber_mutex mutex_;
    acl::fiber_cond readers_ready_, writers_ready_;
    std::set<reader_slot*> slots_;
    size_t waiting_readers_ = 0, waiting_writers_ = 0;
    bool writer_ = false;
    std::unique_lock<acl::fiber_mutex> locked() {
        if (!mutex_.lock())
            throw std::runtime_error("execution gate lock failed");
        return std::unique_lock<acl::fiber_mutex>(mutex_, std::adopt_lock);
    }
    bool readers_active() const {
        for (const auto* slot : slots_) if (slot->readers.load()) return true;
        return false;
    }
    void leave_reader(reader_slot& slot) {
        assert(slot.readers.load());
        if (slot.readers.fetch_sub(1) != 1 || !closed_.load()) return;
        auto lock = locked();
        if (waiting_writers_) writers_ready_.notify();
    }
};
// Process lifetime avoids teardown ordering between ACL fiber-local cleanup
// and thread-local reader slots. All per-thread slots unregister on exit.
gate& shared_gate() { static gate* const value = new gate; return *value; }
reader_slot::~reader_slot() {
    if (registered) shared_gate().remove(*this);
}
reader_slot& local_slot() {
    static thread_local reader_slot slot;
    if (!slot.registered) shared_gate().add(slot);
    return slot;
}
} // namespace

long long execution_time_ms() {
    const auto& local = current();
    return local.exclusive ? local.time : wall_time_ms();
}

long long execution_time_us() {
    const auto& local = current();
    return local.exclusive ? local.microseconds : wall_time_us();
}

bool execution_replica_read() { return current().replica_reads != 0; }
void replica_read_guard::enter() { ++current().replica_reads; }
void replica_read_guard::leave() {
    auto& depth = current().replica_reads;
    assert(depth); --depth;
}

execution_guard::execution_guard(bool exclusive) : exclusive_(exclusive) {
    if (exclusive_) shared_gate().enter_exclusive();
    else shared_gate().enter_shared();
}
execution_guard::~execution_guard() {
    if (exclusive_) shared_gate().leave_exclusive();
    else shared_gate().leave_shared();
}

execution_pause::execution_pause() : depth_(shared_gate().pause()) {}
execution_pause::~execution_pause() { shared_gate().resume(depth_); }

} // namespace pkv
