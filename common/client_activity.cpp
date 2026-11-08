#include "client_activity.h"
#include "client_memory.h"
#include <chrono>

namespace pkv {
namespace {
long long seconds_now() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}
} // namespace
size_t client_worker_id() {
    static std::atomic<size_t> next{0};
    static thread_local const size_t worker = next.fetch_add(1);
    return worker;
}
client_activity::client_activity(size_t base, size_t worker)
: base_(base), worker_(worker), last_seen_(seconds_now()) {}
void client_activity::received(size_t bytes, size_t capacity) {
    buffered(bytes);
    receive_capacity(capacity);
}
void client_activity::buffered(size_t bytes) {
    received_ += bytes;
    input_bytes_.store(received_, std::memory_order_relaxed);
    last_seen_.store(seconds_now(), std::memory_order_relaxed);
}
void client_activity::sending(const std::string& buffer) {
    output_size_.store(buffer.size(), std::memory_order_relaxed);
    output_capacity_.store(buffer.capacity(), std::memory_order_relaxed);
    output_heap_.store(string_heap_bytes(buffer), std::memory_order_relaxed);
    if (buffer.size() > peak_.load(std::memory_order_relaxed))
        peak_.store(buffer.size(), std::memory_order_relaxed);
    writing_.store(true, std::memory_order_release);
}
void client_activity::writing(size_t bytes) {
    if (bytes > peak_.load(std::memory_order_relaxed))
        peak_.store(bytes, std::memory_order_relaxed);
    writing_.store(true, std::memory_order_release);
}
void client_activity::sent(size_t bytes, bool idle) {
    sent_ += bytes;
    output_bytes_.store(sent_, std::memory_order_relaxed);
    if (bytes) last_seen_.store(seconds_now(), std::memory_order_relaxed);
    writing_.store(false, std::memory_order_release);
    output_size_.store(0, std::memory_order_relaxed);
    output_capacity_.store(0, std::memory_order_relaxed);
    output_heap_.store(0, std::memory_order_relaxed);
    if (idle) idle_arguments();
}
client_runtime client_activity::snapshot() const {
    client_runtime out;
    out.input_bytes = input_bytes_.load(std::memory_order_relaxed);
    out.output_bytes = output_bytes_.load(std::memory_order_relaxed);
    out.commands = commands_.load(std::memory_order_relaxed);
    out.database = database_.load(std::memory_order_relaxed);
    out.arguments = arguments_.load(std::memory_order_relaxed);
    out.output_size = output_size_.load(std::memory_order_relaxed);
    out.output_capacity = output_capacity_.load(std::memory_order_relaxed);
    out.output_heap = output_heap_.load(std::memory_order_relaxed);
    out.peak = peak_.load(std::memory_order_relaxed);
    out.writing = writing_.load(std::memory_order_acquire);
    out.last_seen = last_seen_.load(std::memory_order_relaxed);
    out.command = command_.load(std::memory_order_relaxed);
    out.worker = worker_;
    out.owned_memory = base_ + transport_.load(std::memory_order_relaxed) +
        out.output_heap;
    return out;
}
} // namespace pkv
