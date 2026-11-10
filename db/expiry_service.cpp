#include "stdafx.h"
#include "expiry_service.h"
#include "database_pool.h"
#ifdef HAS_TBB
#include "mdb/mdb_tbb.h"
#endif
#include <chrono>
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
            run();
        } catch (const std::exception& error) {
            logger_error("expiry worker failed: %s", error.what());
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
#ifdef HAS_TBB
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
#endif
    size_t remaining = batch_;
    bool busy = false;
    for (size_t i = 0; i < databases.size() && remaining && !stop_.load();
        ++i) {
        if (cursor_ >= databases.size()) cursor_ = 0;
        auto& database = databases[cursor_++];
#ifdef HAS_TBB
        if (auto* native = dynamic_cast<mdb_tbb*>(database.get()))
            native->trim_idle_storage(static_cast<uint64_t>(now));
#endif
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
            std::this_thread::sleep_for(std::chrono::milliseconds(
                failed_ ? 1000 : busy ? 1 : interval_));
    }
}
} // namespace pkv
