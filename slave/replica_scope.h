#pragma once
#include "common/execution_gate.h"

namespace pkv {
class slave_watcher;

// Exclusive admission also linearizes subscriber registration. Nested scopes
// borrow the active collection; ordinary source commands keep shared admission.
class replica_scope {
public:
    explicit replica_scope(slave_watcher* watcher);
    ~replica_scope();
    replica_scope(const replica_scope&) = delete;
    replica_scope& operator=(const replica_scope&) = delete;
private:
    execution_guard admission_;
    slave_watcher* watcher_;
    bool owner_;
    int exceptions_;
};
} // namespace pkv
