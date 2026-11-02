#include "stdafx.h"
#include "common/key_wait.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <new>

static bool reject_new = false;
static size_t requested = 0;
void* operator new(std::size_t size) {
    if (reject_new) throw std::bad_alloc();
    if (void* p = std::malloc(size ? size : 1)) {
        requested += size; return p;
    }
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
#if __cplusplus >= 201402L
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
#endif
using clock_type = std::chrono::steady_clock;
static double seconds(clock_type::time_point start) {
    return std::chrono::duration<double>(clock_type::now() - start).count();
}
int main(int argc, char** argv) {
    using namespace pkv;
    acl::acl_cpp_init();
    if (argc != 3) return 2;
    const std::string mode(argv[1]);
    const size_t count = std::strtoul(argv[2], nullptr, 10);
    if (!count || count > 65536) return 2;
    const bool mixed = mode == "mixed" || mode == "alternating";
    const bool cancel = mode == "cancel";
    if (!mixed && !cancel && mode != "list" && mode != "zset") return 2;
    const auto kind = mixed || mode == "zset" ? key_wait_kind::zset :
        key_wait_kind::list;
    auto queue = std::make_shared<key_wait_registry>(true);
    const std::string key("hot-key");
    const std::vector<std::string> keys{key};
    std::vector<key_wait_token> selected, others;
    selected.reserve(count); others.reserve(mixed ? count : 0);
    const auto bytes = requested;
    auto start = clock_type::now();
    for (size_t i = 0; i < count; ++i) {
        if (mixed) others.push_back(queue->subscribe(keys,
            key_wait_kind::list));
        if (mode == "alternating")
            selected.push_back(queue->subscribe(keys, kind));
    }
    if (mode != "alternating")
        for (size_t i = 0; i < count; ++i)
            selected.push_back(queue->subscribe(keys, kind));
    const auto setup = seconds(start);
    const auto allocation = requested - bytes;
    const key_ready_change change(&key, true, -1, kind);
    reject_new = true;
    start = clock_type::now();
    if (!cancel) queue->publish(&change, 1);
    const auto publication = seconds(start);
    start = clock_type::now();
    for (auto& token : selected) {
        key_wait_event event;
        if (!cancel && (!token->wait(0, event) || !event.ready ||
            token->key(event) != key)) return 3;
        token.reset();
    }
    const auto drain = seconds(start);
    start = clock_type::now();
    for (auto& token : others) token.reset();
    const auto cleanup = seconds(start);
    reject_new = false;
    if (queue->subscribers()) return 4;
    std::printf("{\"mode\":\"%s\",\"clients\":%zu,\"setup_seconds\":%.9f,"
        "\"publication_seconds\":%.9f,\"drain_seconds\":%.9f,"
        "\"cleanup_seconds\":%.9f,\"setup_cpp_requested_bytes\":%zu}\n",
        mode.c_str(), count, setup, publication, drain, cleanup, allocation);
}
