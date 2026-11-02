#include "stdafx.h"
#include "common/key_wait.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <new>

static std::atomic<long long> live{0};
static thread_local long budget = -1;
void* operator new(std::size_t size) {
    if (!budget) throw std::bad_alloc();
    if (budget > 0) --budget;
    if (void* p = std::malloc(size ? size : 1)) {
        live.fetch_add(1); return p;
    }
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* p) noexcept {
    if (p) { live.fetch_sub(1); std::free(p); }
}
void operator delete[](void* p) noexcept { ::operator delete(p); }
#if __cplusplus >= 201402L
void operator delete(void* p, std::size_t) noexcept { ::operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept { ::operator delete(p); }
#endif
int main() {
    using namespace pkv;
    acl::acl_cpp_init();
    std::vector<std::string> keys{std::string("binary\0key", 10),
        "duplicate", "duplicate"};
    for (unsigned i = 0; i < 12; ++i)
        keys.push_back(std::string(80, char('a' + i)));
    unsigned failed = 0, succeeded = 0, leaks = 0;
    long long leaked = 0;
    for (bool existing : {false, true}) {
        for (auto kind : {key_wait_kind::generic, key_wait_kind::list,
                key_wait_kind::zset}) {
            {
                auto queue = std::make_shared<key_wait_registry>(
                    kind != key_wait_kind::generic);
                auto token = queue->subscribe(keys, kind);
            }
            for (long allowance = 0; allowance < 256; ++allowance) {
                const auto before = live.load();
                const auto waiting = key_wait_registry::waiting(kind);
                {
                    auto queue = std::make_shared<key_wait_registry>(
                        kind != key_wait_kind::generic);
                    key_wait_token head;
                    if (existing)
                        head = queue->subscribe({keys.front()}, kind);
                    const auto expected = existing ? 1U : 0U;
                    try {
                        budget = allowance;
                        auto token = queue->subscribe(keys, kind);
                        budget = -1; token.reset(); ++succeeded;
                    } catch (const std::bad_alloc&) { budget = -1; ++failed; }
                    if (queue->subscribers() != expected ||
                        key_wait_registry::waiting(kind) !=
                            waiting + expected ||
                        (head && !head->first(keys.front()))) return 2;
                }
                const auto delta = live.load() - before;
                if (delta) { ++leaks; leaked += delta; }
            }
        }
    }
    std::printf("Key wait heap budgets: failed=%u succeeded=%u "
        "leaking_cases=%u live_blocks=%lld\n", failed, succeeded, leaks,
        leaked);
    if (!failed || !succeeded || leaks || leaked) return 1;
    std::puts("Subscription allocation failures release every C++ block");
}
