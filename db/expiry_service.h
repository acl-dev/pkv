#pragma once
#include <atomic>
#include <memory>
#include <thread>
#include <unordered_set>

namespace pkv {
class database_pool;
class db;
class expiry_service {
public:
    expiry_service(std::shared_ptr<database_pool> databases, size_t batch,
        size_t interval);
    ~expiry_service();
    expiry_service(const expiry_service&) = delete;
    expiry_service& operator=(const expiry_service&) = delete;
private:
    std::shared_ptr<database_pool> databases_;
    const size_t batch_, interval_;
    std::atomic<bool> stop_{false};
    std::unordered_set<db*> unsupported_;
    size_t cursor_ = 0;
    bool failed_ = false;
    std::thread thread_;
    bool tick();
    void run();
};
} // namespace pkv
