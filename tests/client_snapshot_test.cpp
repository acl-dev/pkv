#include "common/client_registry.h"
#include "common/client_state.h"
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <new>
#include <set>
#include <stdexcept>
#include <thread>
#include <type_traits>

static thread_local long budget = -1;
void* operator new(std::size_t size) {
    if (!budget) throw std::bad_alloc();
    if (budget > 0) --budget;
    if (void* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { ::operator delete(p); }
#if __cplusplus >= 201402L
void operator delete(void* p, std::size_t) noexcept { ::operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept { ::operator delete(p); }
#endif
using namespace pkv;
using status = client_state::status;
static_assert(std::is_nothrow_move_assignable<client_snapshot>::value,
    "Publishing staged metadata must not allocate");
static void require(bool ok, const char* reason) {
    if (!ok) { std::cerr << reason << '\n'; std::exit(1); }
}
static void membership() {
    client_registry registry;
    for (long long id = 1; id <= 257; ++id) registry.add(id, 100);
    auto ordered = registry.snapshot();
    auto unsorted = registry.snapshot(false);
    std::set<long long> ids;
    for (const auto& entry : unsorted) ids.insert(entry->id());
    require(ids.size() == ordered.size(), "unsorted membership size");
    auto it = ids.begin();
    for (const auto& entry : ordered)
        require(entry->id() == *it++, "snapshot membership mismatch");
    bool failed = false;
    budget = 0;
    try { registry.snapshot(false); }
    catch (const std::bad_alloc&) { failed = true; }
    budget = -1;
    require(failed && registry.snapshot(false).size() == 257,
        "unordered snapshot OOM retained locks or changed membership");
}
static void allocation_failures() {
    client_state state;
    const std::string large(70000, 'x');
    require(state.set_name(large) == status::ok &&
        state.set_library(false, large) == status::ok &&
        state.set_library(true, large) == status::ok, "large metadata setup");
    client_registry registry;
    auto entry = registry.add(state.id(), 100, &state);
    bool mismatched = false;
    try { registry.add(state.id() + 1, 100, &state); }
    catch (const std::invalid_argument&) { mismatched = true; }
    require(mismatched, "mismatched metadata ID accepted");
    size_t failures = 0, successes = 0;
    for (long available = 0; available < 6; ++available) {
        client_snapshot output;
        output.id = -1; output.protocol = 99; output.name = "old";
        bool failed = false;
        budget = available;
        try { require(entry->read_metadata(output), "metadata unavailable"); }
        catch (const std::bad_alloc&) { failed = true; }
        budget = -1;
        if (failed) {
            ++failures;
            require(output.id == -1 && output.protocol == 99 &&
                output.name == "old" && output.library_name.empty() &&
                output.library_version.empty(), "partial snapshot published");
        } else {
            ++successes;
            require(output.id == state.id() && output.protocol == 2 &&
                output.name == large && output.library_name == large &&
                output.library_version == large, "snapshot copy mismatch");
        }
    }
    require(failures == 3 && successes == 3, "snapshot allocation coverage");
    state.authenticated(false);
    budget = 0;
    const auto changed = state.hello(3, &large);
    budget = -1;
    require(changed == status::no_memory && !state.authenticated() &&
        state.protocol() == 2 && state.name() == large,
        "HELLO allocation failure partially committed");
    require(state.hello(1, nullptr) == status::invalid &&
        state.hello(3, nullptr) == status::ok && state.authenticated(),
        "HELLO protocol validation or absent-name preservation");
    client_snapshot output;
    output.id = -1;
    registry.retire(entry);
    require(!entry->read_metadata(output) && output.id == -1,
        "retired snapshot modified output");
}
static void coherent_updates() {
    client_state state;
    const std::string two("two"), three("three");
    state.hello(2, &two);
    state.set_library(false, "library");
    state.set_library(true, "1.0");
    client_registry registry;
    auto entry = registry.add(state.id(), 100, &state);
    std::atomic<bool> start{false}, done{false};
    std::atomic<size_t> reads{0};
    std::vector<std::thread> readers;
    for (unsigned i = 0; i < 4; ++i) readers.emplace_back([&] {
        while (!start.load()) std::this_thread::yield();
        do {
            client_snapshot view;
            require(entry->read_metadata(view), "live snapshot unavailable");
            const bool pair = (view.protocol == 3 && view.name == three) ||
                (view.protocol == 2 && (view.name == two || view.name.empty()));
            require(pair && view.id == state.id() &&
                view.library_name == "library" && view.library_version ==
                "1.0", "torn HELLO/RESET metadata snapshot");
            ++reads;
        } while (!done.load());
    });
    start = true;
    for (unsigned i = 0; i < 10000; ++i) {
        require(state.hello(3, &three) == status::ok &&
            state.hello(2, &two) == status::ok, "HELLO updates");
        if (!(i % 5)) state.reset(false);
    }
    done = true;
    for (auto& reader : readers) reader.join();
    require(reads.load() >= 4, "snapshot readers did not run");
}
static void retirement() {
    client_registry registry;
    std::unique_ptr<client_state> owner(new client_state);
    owner->set_name("live");
    const auto id = owner->id();
    auto entry = registry.add(id, 100, owner.get());
    std::atomic<size_t> reads{0};
    std::atomic<bool> retired{false};
    std::thread reader([&] {
        while (!retired.load()) {
            client_snapshot output;
            if (entry->read_metadata(output)) {
                require(output.id == id && output.name == "live",
                    "snapshot before retirement");
                ++reads;
            }
        }
        for (unsigned i = 0; i < 1000; ++i) {
            client_snapshot output;
            output.id = -1;
            require(!entry->read_metadata(output) && output.id == -1,
                "snapshot accessed destroyed client");
        }
    });
    while (reads.load() < 100) std::this_thread::yield();
    require(registry.retire(entry), "retirement failed");
    owner.reset();
    retired = true;
    reader.join();
}
int main() {
    membership(); allocation_failures(); coherent_updates(); retirement();
    std::cout << "Client snapshots: membership, OOM, coherence and lifetime "
        "passed\n";
}
