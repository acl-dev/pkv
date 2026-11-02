#include "coder/redis_coder.h"
#include "coder/redis_ocache.h"
#include <atomic>
#include <climits>
#include <cstdlib>
#include <iostream>
#include <new>
#include <string>

static thread_local long budget = -1;
static std::atomic<long> live(0);
void* operator new(std::size_t size) {
    if (!budget) throw std::bad_alloc();
    if (budget > 0) --budget;
    if (void* p = std::malloc(size ? size : 1)) { ++live; return p; }
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* p) noexcept {
    if (p) { --live; std::free(p); }
}
void operator delete[](void* p) noexcept { ::operator delete(p); }
#if __cplusplus >= 201402L
void operator delete(void* p, std::size_t) noexcept { ::operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept { ::operator delete(p); }
#endif
static void require(bool ok, const char* text) {
    if (!ok) { std::cerr << text << '\n'; std::exit(1); }
}
static void exercise(unsigned mode, size_t capacity, const std::string& wire) {
    pkv::redis_ocache cache(capacity);
    pkv::redis_coder parser(cache), builder(cache);
    if (mode != 1) {
        size_t remaining = wire.size();
        parser.update(wire.data(), remaining);
        require(!remaining && parser.get_objects().size() == 2,
            "allocation control parse");
    }
    if (mode != 0) {
        auto& array = builder.create_object();
        array.set_type(pkv::REDIS_OBJ_ARRAY);
        for (unsigned i = 0; i < 24; ++i) {
            auto& pair = array.create_child();
            pair.set_type(pkv::REDIS_OBJ_ARRAY);
            pair.create_child().set_string(std::string(64, 'x'));
            pair.create_child().set_number(i);
        }
        if (mode == 2) parser.move_to(array);
    }
    std::string output;
    require(parser.to_string(output) && builder.to_string(output),
        "allocation control serialize");
}
int main() {
    const std::string wire = "*3\r\n+OK\r\n*2\r\n$1\r\na\r\n:1\r\n$256\r\n" +
        std::string(256, 'v') + "\r\n+NEXT\r\n";
    unsigned total = 0;
    for (unsigned mode = 0; mode < 3; ++mode) {
        for (size_t capacity : {size_t(0), size_t(1), size_t(8), size_t(64)}) {
            const auto before = live.load();
            budget = LONG_MAX;
            exercise(mode, capacity, wire);
            const auto count = LONG_MAX - budget;
            budget = -1;
            require(live == before, "control leaked RESP objects");
            for (long fault = 0; fault <= count; ++fault) {
                budget = fault;
                try { exercise(mode, capacity, wire); }
                catch (const std::bad_alloc&) {}
                budget = -1;
                require(live == before, "failed allocation leaked RESP owner");
                ++total;
            }
        }
    }
    std::cout << "RESP allocation-budget positions: " << total << '\n';
}
