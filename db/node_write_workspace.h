#pragma once
#include <cstddef>
#include <new>
#include <string>
#include <type_traits>

namespace pkv {
namespace detail {

// Move empty capacity out of the pool. Live writers and staged pages always
// own their buffers; only the completed commit scope returns page storage.
class node_write_workspace {
public:
    // Compact map pages can spill out of inline string storage below 80 bytes.
    enum : size_t { max_entries = 64, min_capacity = 24, max_capacity = 1024 };
    static bool fits(size_t size) {
        return size >= min_capacity && size <= max_capacity;
    }
private:
    using cell = typename std::aligned_storage<sizeof(std::string),
        alignof(std::string)>::type;
    struct cache { cell cells[max_entries]; size_t count; unsigned state; };
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
    static cache& local() {
        // Trivial TLS survives later destructors as a shutdown sentinel.
        static thread_local cache value{};
        return value;
    }
    static void initialize(cache& value) {
        static_assert(std::is_nothrow_default_constructible<
            std::string>::value, "empty buffer construction must not throw");
        static thread_local cleanup guard{&value};
        (void)guard;
        for (size_t i = 0; i < max_entries; ++i)
            ::new (static_cast<void*>(&value.cells[i])) std::string;
        value.state = 1;
    }
    std::string bytes_;
    const bool enabled_;
public:
    explicit node_write_workspace(bool enabled = true) : enabled_(enabled) {
        if (!enabled_) return;
        auto& value = local();
        if (!value.state) initialize(value);
        if (value.state == 1 && value.count)
            bytes_.swap(at(value, --value.count));
    }
    node_write_workspace(const node_write_workspace&) = delete;
    node_write_workspace& operator=(const node_write_workspace&) = delete;
    ~node_write_workspace() { if (enabled_) recycle(bytes_); }
    std::string& buffer() { return bytes_; }
    static void recycle(std::string& bytes) noexcept {
        auto& value = local();
        if (value.state != 1 || value.count == max_entries ||
            !fits(bytes.capacity())) return;
        bytes.clear();
        bytes.swap(at(value, value.count++));
    }
    static size_t retained_entries() { return local().count; }
    static size_t retained_capacity() {
        auto& value = local();
        size_t total = 0;
        for (size_t i = 0; i < value.count; ++i)
            total += at(value, i).capacity();
        return total;
    }
};

} // namespace detail
} // namespace pkv
