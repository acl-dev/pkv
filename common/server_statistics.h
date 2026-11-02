#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace pkv {
uint64_t statistics_time_us();
struct command_statistics {
    uint64_t calls = 0, usec = 0, rejected = 0, failed = 0;
};
struct server_counters {
    uint64_t connections = 0, input = 0, output = 0, errors = 0;
    std::vector<command_statistics> commands;
    std::map<std::string, uint64_t> error_codes;
    uint64_t acl[4]{};
};
class worker_statistics {
public:
    explicit worker_statistics(size_t commands);
    ~worker_statistics();
    void connection() { publish(0, 1); }
    void received(size_t bytes) { publish(1, bytes); }
    void sent(size_t bytes) { publish(2, bytes); }
    void complete(size_t command, uint64_t usec, bool failed);
    void complete_observed(size_t command, uint64_t usec, bool failed);
    void reject(size_t command);
    void error(const char* code) noexcept;
    void denied(size_t reason) { if (reason < 4) acl_.add(reason, 1); }
    void append(server_counters& out) const;
private:
    struct counter {
        uint64_t owned[4]{};
        std::atomic<uint64_t> values[4];
        counter() { for (auto& value : values) value.store(0); }
        void add(size_t index, uint64_t value) {
            owned[index] += value;
            values[index].store(owned[index], std::memory_order_relaxed);
        }
    };
    counter totals_, acl_;
    const size_t count_;
    std::unique_ptr<char[]> memory_;
    counter* commands_;
    mutable std::mutex error_mutex_;
    std::map<std::string, uint64_t> errors_;
    static size_t bytes(size_t count);
    void publish(size_t index, uint64_t value) { totals_.add(index, value); }
};
class server_statistics {
public:
    server_statistics();
    worker_statistics& worker(size_t index);
    size_t index(const char* command) const;
    size_t unknown() const { return names_.size(); }
    const std::vector<std::string>& names() const { return names_; }
    server_counters snapshot() const;
    void reset();
private:
    std::vector<std::string> names_;
    std::map<std::string, size_t> indexes_;
    mutable std::mutex mutex_;
    std::map<size_t, std::unique_ptr<worker_statistics>> workers_;
    server_counters baseline_;
    server_counters raw() const;
};
} // namespace pkv
