#pragma once
#include "client_state.h"
#include <atomic>
#include <cstdint>

namespace pkv {
struct client_runtime {
    uint64_t input_bytes = 0, output_bytes = 0, commands = 0;
    size_t database = 0, worker = 0, arguments = 0;
    size_t output_size = 0, output_capacity = 0, output_heap = 0, peak = 0;
    size_t query_size = 0, query_free = 0, owned_memory = 0;
    size_t watches = 0, multi_memory = 0;
    size_t sub = 0, psub = 0, ssub = 0, output_frames = 0;
    long long last_seen = 0, multi = -1;
    bool writing = false, dirty_watch = false;
    bool blocked = false;
    const char* command = "NULL";
};
class client_activity {
public:
    explicit client_activity(size_t base = 0, size_t worker = 0);
    void received(size_t bytes, size_t capacity);
    void receive_capacity(size_t capacity) {
        transport_.store(capacity, std::memory_order_relaxed);
    }
    void buffered(size_t bytes);
    void sending(const std::string& buffer);
    void writing(size_t bytes);
    void sent(size_t bytes, bool idle = true);
    void command(const char* name, size_t bytes) {
        if (!name) name = "NULL";
        if (name != command_cache_) {
            command_cache_ = name;
            command_.store(name, std::memory_order_relaxed);
        }
        arguments(bytes);
    }
    void complete() {
        commands_.store(++processed_, std::memory_order_relaxed);
    }
    void idle_arguments() { arguments(0); }
    void database(size_t index) { database_.store(index); }
    client_runtime snapshot() const;
private:
    void arguments(size_t bytes) {
        if (bytes == arguments_cache_) return;
        arguments_cache_ = bytes;
        arguments_.store(bytes, std::memory_order_relaxed);
    }
    const size_t base_, worker_;
    uint64_t received_ = 0, sent_ = 0, processed_ = 0;
    size_t arguments_cache_ = 0;
    const char* command_cache_ = "NULL";
    std::atomic<uint64_t> input_bytes_{0}, output_bytes_{0}, commands_{0};
    std::atomic<size_t> database_{0}, arguments_{0}, transport_{0};
    std::atomic<size_t> output_size_{0}, output_capacity_{0}, output_heap_{0};
    std::atomic<size_t> peak_{0};
    std::atomic<long long> last_seen_;
    std::atomic<bool> writing_{false};
    std::atomic<const char*> command_{"NULL"};
};
struct client_inspector {
    using reader = client_runtime (*)(const void*, long long);
    const void* context;
    reader read;
    client_inspector(const void* value = nullptr, reader probe = nullptr)
        : context(value), read(probe) {}
};
struct client_view {
    client_snapshot metadata;
    client_runtime runtime;
    intptr_t descriptor = -1;
    unsigned closing = 0;
    bool unix_socket = false;
};
size_t client_worker_id();
} // namespace pkv
