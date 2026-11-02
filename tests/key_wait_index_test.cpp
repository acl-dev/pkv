#include "stdafx.h"
#include "common/key_wait.h"
#include <cstdio>
#include <cstdlib>
#include <new>

static bool reject_new = false;
void* operator new(std::size_t size) {
    if (reject_new) throw std::bad_alloc();
    if (void* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
#if __cplusplus >= 201402L
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
#endif
using namespace pkv;
static void require(bool ok, const char* reason) {
    if (!ok) { std::fprintf(stderr, "%s\n", reason); std::exit(1); }
}
static void permutations() {
    const std::string key("indexed\0key", 11);
    auto queue = std::make_shared<key_wait_registry>(true);
    unsigned random = 7;
    for (unsigned round = 0; round < 32; ++round) {
        std::vector<key_wait_token> tokens;
        std::vector<key_wait_kind> kinds;
        for (unsigned i = 0; i < 96; ++i) {
            random = random * 1664525U + 1013904223U;
            const auto kind = static_cast<key_wait_kind>((random >> 8) % 3);
            kinds.push_back(kind);
            tokens.push_back(queue->subscribe({key, key}, kind));
        }
        for (size_t i = 0; i < tokens.size(); ++i)
            if ((i + round) % 5 == 0) tokens[i].reset();
        for (auto kind : {key_wait_kind::generic, key_wait_kind::list,
                key_wait_kind::zset}) {
            bool first = true;
            for (size_t i = 0; i < tokens.size(); ++i) {
                if (!tokens[i] || kinds[i] != kind) continue;
                require(tokens[i]->first(key) == first,
                    "middle cancellation corrupted a type head");
                first = false;
            }
        }
        for (unsigned offset = 0; offset < 3; ++offset) {
            const auto kind = static_cast<key_wait_kind>((round + offset) % 3);
            const key_ready_change change(&key, true, -1, kind);
            reject_new = true;
            queue->publish(&change, 1);
            for (size_t i = 0; i < tokens.size(); ++i) {
                if (!tokens[i] || kinds[i] != kind) continue;
                key_wait_event event;
                require(tokens[i]->wait(0, event) && event.ready &&
                    tokens[i]->key(event) == key && tokens[i]->first(key),
                    "cached type head bypassed FIFO");
                tokens[i].reset();
            }
            reject_new = false;
        }
        require(!queue->subscribers(), "type queue reuse leaked membership");
    }
}
static void overlaps() {
    auto queue = std::make_shared<key_wait_registry>(true);
    const std::string a("a"), b("b");
    auto first = queue->subscribe({a, b}, key_wait_kind::list);
    auto middle = queue->subscribe({b, "c"}, key_wait_kind::zset);
    auto last = queue->subscribe({b, a}, key_wait_kind::list);
    middle.reset();
    auto replacement = queue->subscribe({b, "c"}, key_wait_kind::zset);
    require(replacement->first(b) && first->first(a) && !last->first(a),
        "empty type tail was not reusable");
    const key_ready_change zset(&b, true, -1, key_wait_kind::zset);
    const key_ready_change list(&a, true, -1, key_wait_kind::list);
    key_wait_event event;
    reject_new = true;
    queue->publish(&zset, 1);
    require(replacement->wait(0, event) && event.ready &&
        !first->first(b), "mixed-type reservation was bypassed");
    replacement.reset();
    queue->publish(&list, 1);
    require(first->wait(0, event) && event.ready && first->key(event) == a,
        "releasing another type lost a producer");
    first.reset();
    require(last->wait(0, event) && event.ready && last->key(event) == a,
        "multi-key cancellation broke a type chain");
    last.reset();
    reject_new = false;
    require(!queue->subscribers(), "overlap cleanup leaked membership");
}
int main() {
    acl::acl_cpp_init();
    permutations(); overlaps();
    for (auto kind : {key_wait_kind::generic, key_wait_kind::list,
            key_wait_kind::zset})
        require(!key_wait_registry::waiting(kind), "global type count leaked");
    std::puts("Cached type heads, arbitrary removals and FIFO reuse passed");
}
