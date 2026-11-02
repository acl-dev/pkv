#pragma once
#include <cstddef>
#include <memory>

namespace pkv {

// Shared admission preserves ordinary command concurrency. EXEC owns exclusive
// admission. Reentrancy follows the current fiber, not merely its OS thread.
long long execution_time_ms();
long long execution_time_us();
bool execution_replica_read();

// Client reads on a replica may compute expired/migrated/cache values without
// committing their cleanup. Received replication updates remain authoritative.
// Disabled nested guards borrow any existing read scope; this is not a write
// permission check. State follows the current fiber, including across yields.
class replica_read_guard {
public:
    explicit replica_read_guard(bool enabled) : enabled_(enabled) {
        if (enabled_) enter();
    }
    ~replica_read_guard() { if (enabled_) leave(); }
    replica_read_guard(const replica_read_guard&) = delete;
    replica_read_guard& operator=(const replica_read_guard&) = delete;
private:
    void enter();
    void leave();
    bool enabled_;
};

class execution_guard {
public:
    explicit execution_guard(bool exclusive = false);
    ~execution_guard();
    execution_guard(const execution_guard&) = delete;
    execution_guard& operator=(const execution_guard&) = delete;
private:
    bool exclusive_;
};

// Release all nested shared admissions while the current actor waits. Never
// suspend EXEC's exclusive admission or its frozen command clock.
class execution_pause {
public:
    execution_pause();
    ~execution_pause();
    execution_pause(const execution_pause&) = delete;
    execution_pause& operator=(const execution_pause&) = delete;
private:
    size_t depth_;
};

} // namespace pkv
