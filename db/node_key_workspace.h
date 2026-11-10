#pragma once
#include "coder/map_page.h"
#include <new>
#include <type_traits>

namespace pkv {
namespace detail {

// Only empty capacity crosses scopes. Every live node owns its key bytes.
class node_key_workspace : public map_node {
public:
    enum : size_t { max_entries = 64, min_capacity = 24, max_capacity = 128 };
private:
    using cell = typename std::aligned_storage<sizeof(std::string),
        alignof(std::string)>::type;
    struct cache { cell cells[max_entries]; size_t count; unsigned state; };
    static cache& local() {
        static thread_local cache value{};
        return value;
    }
    static std::string& at(cache& value, size_t index) {
        return *reinterpret_cast<std::string*>(&value.cells[index]);
    }
    struct cleanup {
        cache* owner;
        ~cleanup() {
            owner->state = 2;
            for (size_t i = 0; i < max_entries; ++i)
                at(*owner, i).~basic_string();
            owner->count = 0;
        }
    };
    static void initialize(cache& value) {
        static_assert(std::is_nothrow_default_constructible<
            std::string>::value, "empty key buffers must not throw");
        static thread_local cleanup guard{&value};
        (void)guard;
        for (size_t i = 0; i < max_entries; ++i)
            ::new (static_cast<void*>(&value.cells[i])) std::string;
        value.state = 1;
    }
    bool enabled_;
public:
    explicit node_key_workspace(bool enabled, bool borrow = true)
        : enabled_(enabled) {
        if (!enabled || !borrow) return;
        auto& thread_cache = local();
        if (!thread_cache.state) initialize(thread_cache);
        if (thread_cache.state == 1 && thread_cache.count)
            key.inline_value.swap(at(thread_cache, --thread_cache.count));
    }
    node_key_workspace(const node_key_workspace&) = delete;
    node_key_workspace& operator=(const node_key_workspace&) = delete;
    bool enabled() const { return enabled_; }
    ~node_key_workspace() {
        if (enabled_) recycle(key.inline_value);
    }
    static void recycle(std::string& bytes) noexcept {
        if (bytes.capacity() < min_capacity ||
            bytes.capacity() > max_capacity) return;
        auto& value = local();
        if (!value.state) initialize(value);
        if (value.state != 1 || value.count == max_entries) return;
        bytes.clear();
        bytes.swap(at(value, value.count++));
    }
    static size_t retained_entries() {
        auto& value = local();
        return value.state == 1 ? value.count : 0;
    }
    static size_t retained_capacity() {
        auto& value = local();
        size_t total = 0;
        if (value.state == 1)
            for (size_t i = 0; i < value.count; ++i)
                total += at(value, i).capacity();
        return total;
    }
};

} // namespace detail
} // namespace pkv
