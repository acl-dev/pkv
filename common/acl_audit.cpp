#include "acl_audit.h"
#include <algorithm>
#include <climits>

namespace pkv {
namespace {
bool matches(const acl_event& a, const acl_event& b) {
    const auto delta = a.updated >= b.updated ?
        static_cast<uint64_t>(a.updated) - static_cast<uint64_t>(b.updated) :
        static_cast<uint64_t>(b.updated) - static_cast<uint64_t>(a.updated);
    return delta <= 60000 && a.reason == b.reason && a.context == b.context &&
        a.object == b.object && a.username == b.username;
}
} // namespace
void acl_audit::trim() {
    while (entries_.size() > limit_.load()) entries_.pop_back();
}
void acl_audit::limit(size_t value) {
    std::lock_guard<std::mutex> lock(mutex_);
    limit_.store(value); trim();
}
void acl_audit::defer_limit(size_t value) {
    std::lock_guard<std::mutex> lock(mutex_);
    limit_.store(value);
}
void acl_audit::trim_disabled() noexcept {
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!enabled()) entries_.clear();
    } catch (...) { lost(); }
}
void acl_audit::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.clear();
}
bool acl_audit::record(const acl_event& event) noexcept {
    if (!enabled()) { trim_disabled(); return true; }
    try {
        // Allocate the complete entry/node before touching retained entries.
        std::list<acl_event> staged;
        staged.push_back(event);
        std::lock_guard<std::mutex> lock(mutex_);
        if (!enabled()) { entries_.clear(); return true; }
        size_t scanned = 0;
        for (auto it = entries_.begin(); it != entries_.end() && scanned < 10;
            ++it, ++scanned) {
            if (!matches(*it, event)) continue;
            it->client_info.swap(staged.front().client_info);
            it->updated = std::max(it->updated, event.updated);
            if (it->count < LLONG_MAX) ++it->count;
            entries_.splice(entries_.begin(), entries_, it);
            return true;
        }
        if (next_id_ > static_cast<uint64_t>(LLONG_MAX)) {
            lost(); return false;
        }
        auto& entry = staged.front();
        entry.count = 1; entry.created = entry.updated;
        entry.id = static_cast<long long>(next_id_++);
        entries_.splice(entries_.begin(), staged);
        trim();
        return true;
    } catch (...) { lost(); return false; }
}
std::vector<acl_event> acl_audit::snapshot(size_t count) const {
    std::lock_guard<std::mutex> lock(mutex_);
    count = std::min(count, entries_.size());
    std::vector<acl_event> result;
    result.reserve(count);
    for (const auto& entry : entries_) {
        if (result.size() == count) break;
        result.push_back(entry);
    }
    return result;
}
} // namespace pkv
