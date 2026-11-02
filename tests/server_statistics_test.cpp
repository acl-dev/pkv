#include "common/server_statistics.h"
#include "common/info_format.h"
#include <atomic>
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
using namespace pkv;
static void require(bool ok, const char* reason) {
    if (!ok) { std::cerr << reason << '\n'; std::exit(1); }
}
static void counters() {
    server_statistics stats;
    const auto get = stats.index("GET"), set = stats.index("set");
    require(get != stats.unknown() && stats.index("bad") == stats.unknown(),
        "metric lookup");
    auto& worker = stats.worker(0);
    require(&worker == &stats.worker(0), "unstable worker reference");
    worker.connection(); worker.received(17); worker.sent(23);
    worker.complete(get, 3, false); worker.complete(set, 5, true);
    worker.reject(get); worker.error("WRONGTYPE"); worker.denied(2);
    auto value = stats.snapshot();
    require(value.connections == 1 && value.input == 17 && value.output == 23
        && value.errors == 1 && value.commands[get].calls == 1 &&
        value.commands[get].usec == 3 && value.commands[get].rejected == 1 &&
        value.commands[set].failed == 1 &&
        value.error_codes["WRONGTYPE"] == 1, "counter values");
    stats.reset(); value = stats.snapshot();
    require(!value.connections && !value.errors && !value.commands[get].calls
        && value.acl[2] == 1, "reset scope");
    worker.complete(get, 2, false); worker.received(7);
    value = stats.snapshot();
    require(value.commands[get].calls == 1 && value.commands[get].usec == 2
        && value.input == 7 && value.acl[2] == 1, "post-reset delta");
    std::string info;
    info_commands(info, value, stats.names());
    require(info.find("cmdstat_get:calls=1,usec=2,") != std::string::npos,
        "command formatting");
}
static void failures() {
    size_t failed = 0, passed = 0;
    for (long n = 0; n < 32; ++n) {
        server_statistics stats;
        const auto metric = stats.index("get");
        auto& worker = stats.worker(0);
        worker.complete(metric, 7, false); worker.error("ERR");
        bool oom = false; budget = n;
        try { stats.reset(); } catch (const std::bad_alloc&) { oom = true; }
        budget = -1;
        const auto result = stats.snapshot();
        if (oom) {
            ++failed;
            require(result.commands[metric].calls == 1 && result.errors == 1,
                "failed reset published baseline");
        } else {
            ++passed;
            require(!result.commands[metric].calls && !result.errors,
                "reset failed to publish");
        }
    }
    require(failed && passed, "reset OOM coverage");
    worker_statistics worker(1);
    budget = 0; worker.error("ERR"); budget = -1;
    server_counters result; worker.append(result);
    require(result.errors == 1 && result.error_codes.empty(),
        "error tracking OOM changed total");
}
static void concurrent() {
    server_statistics stats;
    const auto get = stats.index("get");
    std::atomic<bool> done{false};
    std::thread reader([&] {
        while (!done.load()) {
            const auto value = stats.snapshot();
            require(value.commands[get].usec == value.commands[get].calls * 3,
                "torn observed command row");
        }
    });
    std::vector<std::thread> threads;
    for (size_t i = 0; i < 8; ++i) threads.emplace_back([&, i] {
        auto& worker = stats.worker(i);
        worker.connection();
        for (size_t j = 0; j < 10000; ++j)
            worker.complete_observed(get, 3, false);
    });
    for (auto& thread : threads) thread.join();
    done = true; reader.join();
    const auto value = stats.snapshot();
    require(value.connections == 8 && value.commands[get].calls == 80000 &&
        value.commands[get].usec == 240000, "lost worker counts");
}
static void identity_and_sections() {
    process_identity first, second;
    require(first.run_id().size() == 40 && first.run_id() != second.run_id()
        && first.pid() > 0, "process identity");
    std::string server("SERVER"), commands("commandstats"), unknown("bad");
    const info_sections sections({&server, &commands});
    require(sections.has("server") && sections.has("commandstats") &&
        !sections.has("stats"), "section selection");
    const info_sections empty({&unknown});
    require(!empty.has("server"), "unknown section");
    const auto process = sample_process();
    require(process.cpu_known && process.rss_known && process.rss > 0,
        "process sample");
}
int main() {
    counters(); failures(); concurrent(); identity_and_sections();
    std::cout << "INFO worker counters, reset OOM, snapshots and "
        "process passed\n";
}
