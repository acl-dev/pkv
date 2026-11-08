#pragma once
#include <cstddef>
#include <cstdint>
#include <new>
#include <string>
#include <type_traits>

namespace pkv {
namespace detail {

// Memoize pure validation only. View must contain owned scalar metadata, and
// remember() requires bytes already proven valid under the supplied bound.
// A full byte comparison, never a hash or page address, guards every reuse.
template<class View, size_t MaxBytes = 2048, bool ReserveMaximum = true>
class validated_page_cache {
public:
    enum : size_t { max_bytes = MaxBytes, max_capacity = MaxBytes * 2 };
private:
    static_assert(std::is_trivial<View>::value,
        "validation metadata must be trivial");
    struct entry {
        std::string bytes;
        View view{};
        uint64_t bound = 0;
        bool valid = false, busy = false;
    };
    struct slot {
        typename std::aligned_storage<sizeof(entry), alignof(entry)>::type data;
        unsigned state;
    };
    static slot& local() {
        static thread_local slot value{};
        return value;
    }
    static entry& contents(slot& value) {
        return *reinterpret_cast<entry*>(&value.data);
    }
    struct cleanup {
        slot* owner;
        ~cleanup() {
            owner->state = 2;
            contents(*owner).~entry();
        }
    };
    entry* active_ = nullptr;
public:
    explicit validated_page_cache(bool enabled = true) {
        if (enabled) activate();
    }
    void activate() {
        if (active_) return;
        auto& value = local();
        if (!value.state) {
            static_assert(std::is_nothrow_default_constructible<entry>::value,
                "empty cache construction must not throw");
            static thread_local cleanup guard{&value};
            (void)guard;
            ::new (static_cast<void*>(&value.data)) entry;
            value.state = 1;
        }
        if (value.state == 1 && !contents(value).busy) {
            active_ = &contents(value);
            active_->busy = true;
        }
    }
    validated_page_cache(const validated_page_cache&) = delete;
    validated_page_cache& operator=(const validated_page_cache&) = delete;
    ~validated_page_cache() {
        if (active_) active_->busy = false;
    }
    bool available() const { return active_ != nullptr; }
    bool lookup(const std::string& bytes, uint64_t bound, View& view) const {
        if (!active_ || !active_->valid || bound < active_->bound ||
            bytes != active_->bytes) return false;
        view = active_->view;
        return true;
    }
    void remember(const std::string& bytes, uint64_t bound,
        const View& view) noexcept {
        if (!active_ || bytes.size() > max_bytes) return;
        try {
            const size_t needed = ReserveMaximum ? size_t(max_bytes) : bytes.size();
            if (active_->bytes.capacity() < needed) {
                std::string pending;
                pending.reserve(needed);
                if (pending.capacity() > max_capacity) return;
                pending.assign(bytes);
                active_->bytes.swap(pending);
            } else active_->bytes.assign(bytes);
            active_->view = view; active_->bound = bound;
            active_->valid = true;
        } catch (...) {
            // Optional acceleration cannot make a valid operation fail.
        }
    }
    static size_t retained_capacity() {
        auto& value = local();
        return value.state == 1 ? contents(value).bytes.capacity() : 0;
    }
};

} // namespace detail
} // namespace pkv
