#include "common/list_values_workspace.h"
#include <cstdlib>
#include <iostream>
#include <new>
#include <thread>

static thread_local long budget = -1;
void* operator new(std::size_t size) {
    if (!budget) throw std::bad_alloc();
    if (budget > 0) --budget;
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

using workspace = pkv::detail::list_values_workspace;
static void require(bool ok, const char* message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}

static void ownership() {
    const std::string binary("a\0b", 3);
    {
        workspace outer;
        auto& values = outer.values();
        values.resize(2);
        values[0] = binary;
        values[1].assign(1024, 'x');
        {
            workspace nested;
            require(nested.values().empty(), "nested lease reused live input");
            nested.values().assign(4, "nested");
            require(values[0] == binary && values[1].size() == 1024,
                "nested lease changed outer input");
        }
        require(values.size() == 2 && values[0] == binary,
            "nested destruction changed outer input");
        require(workspace::retained_bytes() == 0, "active bytes marked idle");
    }
    {
        workspace next;
        for (const auto& value : next.values())
            require(value.empty(), "released lease retained input contents");
        next.values().resize(1);
        next.values()[0] = "short";
        require(next.values().size() == 1, "stale argument count");
    }
}

static void bounds() {
    for (size_t count : {size_t(0), size_t(1), size_t(128), size_t(129)}) {
        {
            workspace lease;
            lease.values().resize(count);
            for (auto& value : lease.values()) value.assign(1024, 'x');
        }
        require(workspace::retained_bytes() <= workspace::max_bytes,
            "idle storage exceeded bound");
        if (count >= 128)
            require(workspace::retained_bytes() == 0,
                "oversized workspace retained");
    }
    {
        workspace lease;
        lease.values().resize(1);
        lease.values()[0].assign(workspace::max_bytes + 1, 'x');
    }
    require(workspace::retained_bytes() == 0, "large value retained");
}

static void allocation_failure() {
    const std::string payload(1024, 'p');
    {
        workspace warm;
        warm.values().resize(1);
        warm.values()[0] = payload;
    }
    // A warmed lease can be reused without allocating its vector or payload.
    budget = 0;
    {
        workspace warm;
        warm.values().resize(1);
        warm.values()[0] = payload;
    }
    budget = -1;
    bool failed = false;
    try {
        workspace lease;
        budget = 0;
        lease.values().resize(1000);
    } catch (const std::bad_alloc&) { failed = true; }
    budget = -1;
    require(failed, "allocation failure was not exercised");
    // Unwinding must release the slot; a stuck busy flag would allocate a
    // fallback vector here and fail again.
    budget = 0;
    {
        workspace recovered;
        recovered.values().resize(1);
        recovered.values()[0] = payload;
    }
    budget = -1;
}

int main() {
    ownership();
    bounds();
    allocation_failure();
    std::thread first([] { ownership(); bounds(); allocation_failure(); });
    std::thread second([] { ownership(); bounds(); allocation_failure(); });
    first.join();
    second.join();
    std::cout << "List values workspace passed\n";
}
