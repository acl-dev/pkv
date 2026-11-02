#pragma once
#include "key_wait.h"
namespace pkv {
// The owning actor must retain exclusive execution admission. All allocation
// happens before storage writes; destruction publishes committed final states.
class key_wait_batch {
public:
    key_wait_batch();
    ~key_wait_batch();
    key_wait_batch(const key_wait_batch&) = delete;
    key_wait_batch& operator=(const key_wait_batch&) = delete;
    static void prepare(key_wait_registry& registry,
        const key_ready_change* changes, size_t count);
    static bool record(key_wait_registry& registry,
        const key_ready_change* changes, size_t count) noexcept;
private:
    struct state;
    std::unique_ptr<state> state_;
    static key_wait_batch* current() noexcept;
};
} // namespace pkv
