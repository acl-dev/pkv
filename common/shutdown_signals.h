#pragma once
#include <functional>

namespace pkv {
// Block in main before any worker is created. A normal thread consumes these
// process signals; no DB, logger or scheduler work runs in a signal handler.
bool block_shutdown_signals();
// Start before ACL/config/native initialization. Cancellation before enabling
// the callback exits without destroying partially initialized objects. No
// application commands have been accepted yet; native recovery may be needed.
bool start_shutdown_signals();
// Publish only after initialization; all subsequent stops close native stores.
bool enable_shutdown_signals(const std::function<void(int)>& stop);
} // namespace pkv
