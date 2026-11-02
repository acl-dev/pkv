#pragma once
#include <chrono>
#include <cstdint>
#include <string>

namespace pkv {
struct process_sample {
    bool heap_known = false, rss_known = false, cpu_known = false;
    uint64_t heap = 0, rss = 0;
    double user = 0, system = 0, child_user = 0, child_system = 0;
    std::string allocator;
};
class process_identity {
public:
    process_identity();
    void initialize();
    uint64_t uptime() const;
    long long pid() const { return pid_; }
    const std::string& run_id() const { return run_id_; }
    const std::string& os() const { return os_; }
private:
    std::chrono::steady_clock::time_point start_;
    long long pid_ = 0;
    std::string run_id_, os_;
};
process_sample sample_process();
} // namespace pkv
