#pragma once
#include "db/page_transaction.h"
#include <array>
#include <vector>
#include <algorithm>

namespace pkv {
namespace detail {

// Two inline buffers and bounded spill storage share one worker-local lease.
// Nested actors use private storage. Owned preparations opt out of the cache.
class replacement_workspace {
    using entry = std::pair<std::string*, std::string>;
    struct slot {
        bool busy = false;
        std::array<std::string, 2> bytes;
        std::vector<std::string> spill;
    };
    static slot& local() {
        static thread_local slot value;
        return value;
    }
    std::array<entry, 2> values_{};
    slot* cached_ = nullptr;
    unsigned used_ = 0;
public:
    enum : size_t { max_spill_entries = 64, max_spill_capacity = 1024 };
    explicit replacement_workspace(bool recycle) {
        if (!recycle) return;
        auto& value = local();
        if (value.busy) return;
        value.busy = true; cached_ = &value;
    }
    replacement_workspace(const replacement_workspace&) = delete;
    replacement_workspace& operator=(const replacement_workspace&) = delete;
    ~replacement_workspace() {
        if (!cached_) return;
        for (size_t i = 0; i < values_.size(); ++i) {
            if (!(used_ & (1U << i))) continue;
            auto& bytes = values_[i].second;
            if (bytes.capacity() <= page_transaction::max_value_bytes)
                cached_->bytes[i].swap(bytes);
            cached_->bytes[i].clear();
        }
        cached_->busy = false;
    }
    size_t size() const { return values_.size(); }
    entry& take(size_t index) {
        if (cached_) {
            values_[index].second.swap(cached_->bytes[index]);
            used_ |= 1U << index;
        }
        return values_[index];
    }
    void stage(size_t index, std::string& target, const std::string& value,
        bool compact = false) {
        auto& replacement = take(index);
        const auto capacity = replacement.second.capacity();
        if ((capacity < value.size() &&
                (compact || value.size() >= page_transaction::max_value_bytes / 8)) ||
            (compact && capacity > value.size() + value.size() / 4)) {
            // Sealed pages and large values should not inherit growth slack.
            std::string exact(value);
            replacement.second.swap(exact);
        } else replacement.second = value;
        replacement.first = &target;
    }
    entry& operator[](size_t index) { return values_[index]; }
    void stage_spill(std::vector<entry>& values, std::string& target,
        const std::string& value, size_t hint) {
        if (values.empty()) values.reserve(hint);
        const auto index = values.size();
        if (!cached_ || index >= max_spill_entries) {
            values.emplace_back(&target, value);
            return;
        }
        auto& buffers = cached_->spill;
        if (buffers.empty()) buffers.reserve(max_spill_entries);
        // Exact-sized replacements can occupy earlier slots without using
        // the cache. Construct the gap before indexing the next slot.
        if (buffers.size() <= index) buffers.resize(index + 1);
        std::string prepared;
        prepared.swap(buffers[index]);
        // Keep publication capacity bounded when switching payload sizes.
        if (prepared.capacity() < value.size() ||
            prepared.capacity() - value.size() > value.size() / 4 + 32) {
            std::string exact(value);
            prepared.swap(exact);
        } else prepared = value;
        values.emplace_back(&target, std::move(prepared));
    }
    void release_spill(std::vector<entry>& values) noexcept {
        if (!cached_) return;
        auto& buffers = cached_->spill;
        const auto count = std::min(values.size(), buffers.size());
        for (size_t i = 0; i < count; ++i) {
            auto& bytes = values[i].second;
            if (bytes.capacity() <= max_spill_capacity) buffers[i].swap(bytes);
            buffers[i].clear();
        }
    }
    static size_t retained_spill_entries() { return local().spill.size(); }
    static size_t retained_spill_capacity() {
        size_t total = 0;
        for (const auto& bytes : local().spill) total += bytes.capacity();
        return total;
    }
    static size_t retained_value_capacity() {
        const auto& value = local();
        return value.bytes[0].capacity() + value.bytes[1].capacity();
    }
};

} // namespace detail
} // namespace pkv
