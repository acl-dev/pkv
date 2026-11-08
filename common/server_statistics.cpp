#include "server_statistics.h"
#include "command_catalog.h"
#include "statistics_clock.h"
#if defined(__APPLE__) && defined(__SIZEOF_INT128__) && \
    defined(__ENVIRONMENT_MAC_OS_X_VERSION_MIN_REQUIRED__) && \
    __ENVIRONMENT_MAC_OS_X_VERSION_MIN_REQUIRED__ >= 101200
#include <mach/mach_time.h>
#define PKV_CONTINUOUS_STATISTICS_CLOCK 1
#endif
#include <chrono>
#include <limits>
#include <new>
#include <stdexcept>

namespace pkv {
uint64_t statistics_time_us() {
#ifdef PKV_CONTINUOUS_STATISTICS_CLOCK
    static const detail::statistics_clock_ratio scale = [] {
        mach_timebase_info_data_t info{};
        if (mach_timebase_info(&info) != KERN_SUCCESS)
            return detail::statistics_clock_ratio(0, 0);
        return detail::statistics_clock_ratio(info.numer, info.denom);
    }();
    if (scale.denominator)
        return scale.microseconds(mach_continuous_time());
#endif
    return static_cast<uint64_t>(std::chrono::duration_cast<
        std::chrono::microseconds>(std::chrono::steady_clock::now().
        time_since_epoch()).count());
}
size_t worker_statistics::bytes(size_t count) {
    if (count > (std::numeric_limits<size_t>::max() - 63) / sizeof(counter))
        throw std::length_error("statistics command capacity overflow");
    return count * sizeof(counter) + 63;
}
worker_statistics::worker_statistics(size_t count)
: count_(count), memory_(new char[bytes(count)]) {
    void* address = memory_.get();
    size_t space = count * sizeof(counter) + 63;
    commands_ = static_cast<counter*>(std::align(64, count * sizeof(counter),
        address, space));
    if (!commands_) throw std::bad_alloc();
    for (size_t i = 0; i < count_; ++i) new (commands_ + i) counter;
}
worker_statistics::~worker_statistics() {
    for (size_t i = 0; i < count_; ++i) commands_[i].~counter();
}
void worker_statistics::complete(size_t command, uint64_t usec, bool failed) {
    if (command >= count_) return;
    auto& value = commands_[command];
    value.add(0, 1); value.add(1, usec);
    if (failed) value.add(3, 1);
}
void worker_statistics::complete_observed(size_t command, uint64_t usec,
    bool failed) {
    std::lock_guard<std::mutex> lock(error_mutex_);
    complete(command, usec, failed);
}
void worker_statistics::reject(size_t command) {
    if (command < count_) commands_[command].add(2, 1);
}
void worker_statistics::error(const char* code) noexcept {
    bool counted = false;
    try {
        std::lock_guard<std::mutex> lock(error_mutex_);
        publish(3, 1); counted = true;
        ++errors_[code];
    } catch (...) { if (!counted) publish(3, 1); }
}
void worker_statistics::append(server_counters& out) const {
    if (out.commands.size() < count_) out.commands.resize(count_);
    std::lock_guard<std::mutex> lock(error_mutex_);
    for (size_t i = 0; i < 4; ++i)
        out.acl[i] += acl_.values[i].load(std::memory_order_relaxed);
    out.connections += totals_.values[0].load(std::memory_order_relaxed);
    out.input += totals_.values[1].load(std::memory_order_relaxed);
    out.output += totals_.values[2].load(std::memory_order_relaxed);
    out.errors += totals_.values[3].load(std::memory_order_relaxed);
    for (size_t i = 0; i < count_; ++i) {
        const auto& values = commands_[i].values;
        out.commands[i].calls += values[0].load(std::memory_order_relaxed);
        out.commands[i].usec += values[1].load(std::memory_order_relaxed);
        out.commands[i].rejected += values[2].load(std::memory_order_relaxed);
        out.commands[i].failed += values[3].load(std::memory_order_relaxed);
    }
    for (const auto& error : errors_)
        out.error_codes[error.first] += error.second;
}
server_statistics::server_statistics() {
    for (const auto* entry : command_catalog_entries()) {
        indexes_.emplace(entry->name, names_.size());
        names_.push_back(entry->name);
    }
    baseline_.commands.resize(names_.size());
}
size_t server_statistics::index(const char* command) const {
    if (!command) return unknown();
    const auto found = indexes_.find(command_lower(command));
    return found == indexes_.end() ? unknown() : found->second;
}
worker_statistics& server_statistics::worker(size_t index) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto found = workers_.find(index);
    if (found == workers_.end()) {
        std::unique_ptr<worker_statistics> value(new worker_statistics(
            names_.size()));
        found = workers_.emplace(index, std::move(value)).first;
    }
    return *found->second;
}
server_counters server_statistics::raw() const {
    server_counters result;
    result.commands.resize(names_.size());
    for (const auto& worker : workers_) worker.second->append(result);
    return result;
}
server_counters server_statistics::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto result = raw();
    result.connections -= baseline_.connections;
    result.input -= baseline_.input; result.output -= baseline_.output;
    result.errors -= baseline_.errors;
    for (size_t i = 0; i < names_.size(); ++i) {
        auto& value = result.commands[i];
        const auto& base = baseline_.commands[i];
        value.calls -= base.calls; value.usec -= base.usec;
        value.rejected -= base.rejected; value.failed -= base.failed;
    }
    for (auto& error : result.error_codes) {
        const auto prior = baseline_.error_codes.find(error.first);
        if (prior != baseline_.error_codes.end()) error.second -= prior->second;
    }
    return result;
}
void server_statistics::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    auto staged = raw();
    baseline_ = std::move(staged);
}
} // namespace pkv
