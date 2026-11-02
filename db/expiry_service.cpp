#include "stdafx.h"
#include "expiry_service.h"
#include "database_pool.h"
#include <stdexcept>

namespace pkv {
expiry_service::expiry_service(std::shared_ptr<database_pool> databases,
    size_t batch, size_t interval)
: databases_(std::move(databases)), batch_(batch), interval_(interval) {
    if (!databases_ || !batch_ || batch_ > 1024 || !interval_ ||
        interval_ > 1000)
        throw std::invalid_argument("invalid active expiry configuration");
    thread_ = std::thread([this] {
        try {
            go[this] { run(); };
            acl::fiber::schedule();
        } catch (const std::exception& error) {
            logger_error("expiry scheduler failed: %s", error.what());
        }
    });
}
expiry_service::~expiry_service() {
    stop_.store(true);
    if (thread_.joinable()) thread_.join();
}
bool expiry_service::tick() {
    failed_ = false;
    auto databases = databases_->opened();
    size_t remaining = batch_;
    bool busy = false;
    for (size_t i = 0; i < databases.size() && remaining && !stop_.load();
        ++i) {
        if (cursor_ >= databases.size()) cursor_ = 0;
        auto& database = databases[cursor_++];
        if (unsupported_.count(database.get())) continue;
        const auto progress = database->expire_step(remaining);
        if (!progress.supported) {
            unsupported_.insert(database.get()); continue;
        }
        if (!progress.ok) { failed_ = true; return false; }
        remaining -= std::min(remaining, progress.checked);
        busy = busy || progress.more;
    }
    return busy || !remaining;
}
void expiry_service::run() {
    while (!stop_.load()) {
        bool busy = false;
        failed_ = false;
        try { busy = tick(); }
        catch (const std::exception& error) {
            failed_ = true;
            logger_error("active expiry deferred: %s", error.what());
        }
        if (!stop_.load())
            acl::fiber::delay(failed_ ? 1000 : busy ? 1 : interval_);
    }
}
} // namespace pkv
