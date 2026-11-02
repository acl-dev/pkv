#include "common/client_activity.h"
#include "common/client_registry.h"
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <thread>
#include <type_traits>
#include <vector>

using namespace pkv;
static void require(bool ok, const char* reason) {
    if (!ok) { std::cerr << reason << '\n'; std::exit(1); }
}
static void counters() {
    client_activity activity(100, 7);
    activity.received(17, 4096);
    activity.command("GET", 8);
    activity.complete();
    auto view = activity.snapshot();
    require(view.input_bytes == 17 && view.commands == 1 &&
        view.arguments == 8 && view.worker == 7 && view.owned_memory ==
        4196 && std::string(view.command) == "GET", "input/call counters");
    const std::string output(10000, 'x');
    activity.sending(output);
    view = activity.snapshot();
    require(view.writing && view.output_size == 10000 &&
        view.output_capacity >= 10000 && view.output_heap >= 10000 &&
        view.peak == 10000, "pending output resources");
    activity.sent(output.size());
    view = activity.snapshot();
    require(!view.writing && !view.output_size && !view.output_heap &&
        !view.arguments && view.output_bytes == 10000 && view.peak == 10000,
        "completed output counters");
    activity.database(15);
    require(activity.snapshot().database == 15, "selected DB gauge");
}
struct observed {
    mutable std::mutex mutex;
    std::vector<size_t> data;
    static client_runtime read(const void* context, long long now) {
        const auto& source = *static_cast<const observed*>(context);
        std::lock_guard<std::mutex> lock(source.mutex);
        client_runtime out;
        out.last_seen = now / 1000;
        out.query_size = source.data.size();
        out.query_free = source.data.capacity() - source.data.size();
        out.owned_memory = sizeof(source) +
            source.data.capacity() * sizeof(size_t);
        return out;
    }
};
static void inspector_lifetime() {
    client_registry registry;
    client_state metadata;
    std::unique_ptr<observed> owner(new observed);
    auto entry = registry.add(metadata.id(), 100, &metadata,
        client_inspector(owner.get(), &observed::read));
    std::atomic<bool> done{false};
    std::atomic<size_t> reads{0};
    std::thread reader([&] {
        while (!done.load()) {
            client_view view;
            if (entry->read_view(view, 12000)) {
                require(view.runtime.last_seen == 12 &&
                    view.metadata.id == metadata.id(), "inspector view");
                ++reads;
            }
        }
    });
    for (size_t i = 0; i < 2000; ++i) {
        std::lock_guard<std::mutex> lock(owner->mutex);
        owner->data.push_back(i);
        if (i % 3 == 0) owner->data.clear();
    }
    while (!reads.load()) std::this_thread::yield();
    registry.retire(entry);
    owner.reset();
    client_view unchanged;
    unchanged.descriptor = -99;
    for (size_t i = 0; i < 1000; ++i)
        require(!entry->read_view(unchanged, 12000) &&
            unchanged.descriptor == -99, "retired inspector accessed context");
    done = true;
    reader.join();
}
static void live_counters() {
    client_activity activity;
    std::atomic<bool> done{false};
    std::thread writer([&] {
        for (unsigned i = 0; i < 100000; ++i) {
            activity.command("PING", 4);
            activity.complete();
        }
        done = true;
    });
    uint64_t previous = 0;
    do {
        const auto view = activity.snapshot();
        require(view.commands >= previous && view.commands <= 100000,
            "non-monotonic command counter");
        previous = view.commands;
    } while (!done.load());
    writer.join();
    require(activity.snapshot().commands == 100000, "lost command counters");
}
int main() {
    static_assert(std::is_nothrow_move_assignable<client_view>::value,
        "View publication must not allocate");
    counters(); inspector_lifetime(); live_counters();
    std::cout << "Client activity counters, observation and lifetime passed\n";
}
