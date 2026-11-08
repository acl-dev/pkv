#pragma once
#include <algorithm>
#include <climits>
#include <cstddef>
#include <atomic>
#include <string>
#include "client_memory.h"

namespace pkv {
// Only the owning connection mutates the cache. Inspectors read its heap count.
class response_buffer_cache {
public:
    static constexpr size_t limit = 128 * 1024;
    size_t owned() const { return heap_.load(std::memory_order_relaxed); }
    class frame {
    public:
        explicit frame(response_buffer_cache& cache) noexcept : cache_(cache) {
            bytes_.swap(cache_.bytes_);
            cache_.heap_.store(0, std::memory_order_relaxed);
        }
        ~frame() {
            bytes_.clear();
            if (bytes_.capacity() <= limit &&
                bytes_.capacity() > cache_.bytes_.capacity()) {
                bytes_.swap(cache_.bytes_);
                cache_.heap_.store(string_heap_bytes(cache_.bytes_),
                    std::memory_order_relaxed);
            }
        }
        frame(const frame&) = delete;
        frame& operator=(const frame&) = delete;
        std::string& bytes() { return bytes_; }
    private:
        response_buffer_cache& cache_;
        std::string bytes_;
    };
private:
    std::string bytes_;
    std::atomic<size_t> heap_{0};
};

// ACL reports individual writes as int. Keep every call within that range.
template<class Sender>
bool response_write(size_t length, size_t& written, Sender sender) {
    const size_t limit = std::min<size_t>(INT_MAX, 16ULL * 1024 * 1024);
    written = 0;
    while (written < length) {
        const auto count = std::min(limit, length - written);
        const auto delivered = sender(written, count);
        if (delivered > count) return false;
        written += delivered;
        if (delivered != count) return false;
    }
    return true;
}
} // namespace pkv
