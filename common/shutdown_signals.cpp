#include "stdafx.h"
#include "shutdown_signals.h"
#include <csignal>
#include <mutex>
#include <thread>

namespace pkv {
#ifndef _WIN32
static sigset_t shutdown_set() {
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT); sigaddset(&signals, SIGTERM);
    return signals;
}
struct signal_state {
    std::mutex mutex;
    std::function<void(int)> stop;
    std::thread waiter;
    bool started = false;
};
static signal_state& state() {
    // A waiter can receive a signal during failed-startup static destruction.
    // Its thread, callback and mutex must live until the process dies.
    static auto* value = new signal_state;
    return *value;
}
static void wait_for_shutdown(signal_state& value) {
    const auto signals = shutdown_set();
    int received = 0;
    if (sigwait(&signals, &received) != 0) _exit(1);
    try {
        std::function<void(int)> stop;
        {
            std::lock_guard<std::mutex> guard(value.mutex);
            stop = value.stop;
        }
        // A full stderr pipe must not prevent cancellation during startup.
        if (!stop) _exit(0);
        stop(received);
    } catch (...) { _exit(1); }
    // A terminal callback must not return and leave future signals unhandled.
    _exit(1);
}
#endif
bool block_shutdown_signals() {
#ifdef _WIN32
    return true;
#else
    const auto signals = shutdown_set();
    return pthread_sigmask(SIG_BLOCK, &signals, nullptr) == 0;
#endif
}
bool start_shutdown_signals() {
#ifdef _WIN32
    return true;
#else
    try {
        auto& value = state();
        std::lock_guard<std::mutex> guard(value.mutex);
        if (value.started) return false;
        value.waiter = std::thread([&value] { wait_for_shutdown(value); });
        value.started = true;
        return true;
    } catch (...) { return false; }
#endif
}
bool enable_shutdown_signals(const std::function<void(int)>& stop) {
#ifdef _WIN32
    (void) stop;
    return true;
#else
    try {
        auto& value = state();
        std::lock_guard<std::mutex> guard(value.mutex);
        if (!value.started || value.stop || !stop) return false;
        value.stop = stop;
        return true;
    } catch (...) { return false; }
#endif
}
} // namespace pkv
