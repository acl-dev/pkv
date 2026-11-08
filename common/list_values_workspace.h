#pragma once
#include "client_memory.h"
#include <vector>

namespace pkv {
namespace detail {
// Inputs remain owned throughout the synchronous DAO call and its callbacks.
// A live lease excludes every other actor on this worker from the cache.
class list_values_workspace {
    struct slot {
        bool busy = false;
        std::vector<std::string> values;
        size_t idle_bytes = 0;
    };
    static slot& local() {
        static thread_local slot value;
        return value;
    }
    slot* cached_ = nullptr;
    std::vector<std::string> fallback_;
public:
    static constexpr size_t max_values = 128;
    static constexpr size_t max_bytes = 64 * 1024;
    // Resize/assignment happen after construction, so their exceptions still
    // release the lease. Nested allocation hooks always get private storage.
    list_values_workspace() {
        auto& value = local();
        if (value.busy) return;
        value.busy = true; value.idle_bytes = 0; cached_ = &value;
    }
    list_values_workspace(const list_values_workspace&) = delete;
    list_values_workspace& operator=(const list_values_workspace&) = delete;
    ~list_values_workspace() {
        if (!cached_) return;
        auto& values = cached_->values;
        bool keep = values.capacity() <= max_values;
        size_t bytes = keep ? values.capacity() * sizeof(std::string) : 0;
        if (keep) for (auto& value : values) {
            const auto heap = string_heap_bytes(value);
            if (heap > max_bytes - bytes) { keep = false; break; }
            bytes += heap;
            value.clear();
        }
        if (!keep) std::vector<std::string>().swap(values);
        cached_->idle_bytes = keep ? bytes : 0;
        cached_->busy = false;
    }
    std::vector<std::string>& values() {
        return cached_ ? cached_->values : fallback_;
    }
    // Current worker's idle requested storage, excluding TLS/allocator cost.
    static size_t retained_bytes() {
        const auto& value = local();
        return value.busy ? 0 : value.idle_bytes;
    }
};
} // namespace detail
} // namespace pkv
