#pragma once
#include <cstddef>
#include <limits>
#include <new>
#include <type_traits>
#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define PKV_PAGE_NODE_ASAN 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__) && !defined(PKV_PAGE_NODE_ASAN)
#define PKV_PAGE_NODE_ASAN 1
#endif
#ifdef PKV_PAGE_NODE_ASAN
#include <sanitizer/asan_interface.h>
#endif

namespace pkv {
namespace detail {

// Cache only deallocated tree nodes. Live maps, including nested actors and
// copies, always own disjoint storage. All blocks originate at operator new,
// so another worker may safely free a block into its own local cache.
template <class T>
class page_node_allocator {
    struct cell { cell* next; };
    static void unpoison(void* pointer) noexcept {
#ifdef PKV_PAGE_NODE_ASAN
        __asan_unpoison_memory_region(pointer, sizeof(T));
#endif
    }
    static void poison(void* pointer) noexcept {
#ifdef PKV_PAGE_NODE_ASAN
        __asan_poison_memory_region(static_cast<char*>(pointer) + sizeof(cell),
            sizeof(T) - sizeof(cell));
#endif
    }
    // Trivial storage avoids a destructor-registration guard on each access.
    // state: 0 uninitialized, 1 caching, 2 shutdown (direct allocation/free).
    struct cache { cell* head; size_t count; unsigned state; };
    struct cleanup {
        cache* owner;
        ~cleanup() {
            owner->state = 2;
            while (owner->head) {
                auto* node = owner->head;
                owner->head = node->next;
                unpoison(node); node->~cell(); ::operator delete(node);
            }
            owner->count = 0;
        }
    };
    static cache& local() {
        static thread_local cache value{};
        return value;
    }
    static void initialize(cache& cached) {
        static thread_local cleanup value{&cached};
        (void)value;
        cached.state = 1;
    }
public:
    using value_type = T;
    using is_always_equal = std::true_type;
    using propagate_on_container_move_assignment = std::true_type;
    template <class U> struct rebind { using other = page_node_allocator<U>; };
    page_node_allocator() noexcept = default;
    template <class U>
    page_node_allocator(const page_node_allocator<U>&) noexcept {}
    T* allocate(size_t count) {
        static_assert(alignof(T) <= alignof(std::max_align_t),
            "page nodes require ordinary allocation alignment");
        if (count > max_size()) throw std::bad_alloc();
        if (count == 1 && sizeof(T) >= sizeof(cell) && sizeof(T) <= 256) {
            auto& cached = local();
            if (!cached.state) initialize(cached);
            if (cached.state == 1 && cached.head) {
                auto* node = cached.head;
                cached.head = node->next; --cached.count;
                unpoison(node);
                node->~cell();
                return reinterpret_cast<T*>(node);
            }
        }
        return static_cast<T*>(::operator new(count * sizeof(T)));
    }
    void deallocate(T* pointer, size_t count) noexcept {
        if (!pointer) return;
        if (count == 1 && sizeof(T) >= sizeof(cell) && sizeof(T) <= 256) {
            auto& cached = local();
            if (!cached.state) initialize(cached);
            if (cached.state == 1 && cached.count < 64) {
                cached.head = ::new (static_cast<void*>(pointer)) cell{
                    cached.head};
                ++cached.count;
                poison(pointer);
                return;
            }
        }
        ::operator delete(pointer);
    }
    size_t max_size() const noexcept {
        return std::numeric_limits<size_t>::max() / sizeof(T);
    }
};
template <class T, class U>
bool operator==(const page_node_allocator<T>&,
    const page_node_allocator<U>&) noexcept { return true; }
template <class T, class U>
bool operator!=(const page_node_allocator<T>&,
    const page_node_allocator<U>&) noexcept { return false; }

} // namespace detail
} // namespace pkv
#ifdef PKV_PAGE_NODE_ASAN
#undef PKV_PAGE_NODE_ASAN
#endif
