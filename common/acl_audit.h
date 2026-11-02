#pragma once
#include <atomic>
#include <cstdint>
#include <list>
#include <mutex>
#include <string>
#include <vector>

namespace pkv {
struct acl_event {
    long long count = 1, id = 0, created = 0, updated = 0;
    std::string reason, context, object, username, client_info;
};
class acl_audit {
public:
    explicit acl_audit(size_t limit = 128) : limit_(limit) {}
    bool enabled() const { return limit_.load() != 0; }
    size_t limit() const { return limit_.load(); }
    void limit(size_t value);
    void defer_limit(size_t value);
    void trim_disabled() noexcept;
    void clear();
    // Audit failure must never turn a rejected request into an allowed one.
    bool record(const acl_event& event) noexcept;
    void lost() noexcept { lost_.fetch_add(1, std::memory_order_relaxed); }
    uint64_t dropped() const { return lost_.load(std::memory_order_relaxed); }
    std::vector<acl_event> snapshot(size_t count) const;
private:
    mutable std::mutex mutex_;
    std::list<acl_event> entries_;
    std::atomic<size_t> limit_;
    std::atomic<uint64_t> lost_{0};
    uint64_t next_id_ = 0;
    void trim();
};
} // namespace pkv
