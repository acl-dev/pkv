#include "stdafx.h"
#include "common/execution_gate.h"
#include "db/db.h"
#include <atomic>
#include <chrono>
#include "coder/record_codec.h"
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace pkv;
static void require(bool ok, const char* message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}
static void nesting() {
    {
        execution_guard writer(true);
        execution_guard reader;
        execution_guard inner_writer(true);
        execution_guard inner_reader;
    }
    bool rejected = false;
    {
        execution_guard reader;
        try { execution_guard illegal(true); }
        catch (const std::logic_error&) { rejected = true; }
    }
    require(rejected, "gate accepted a reader upgrade");
    try {
        execution_guard writer(true);
        throw std::runtime_error("unwind");
    } catch (const std::runtime_error&) {}
    execution_guard reader;
}
static void parallel_readers() {
    std::atomic<unsigned> entered{0};
    std::vector<std::thread> threads;
    for (unsigned i = 0; i < 4; ++i) threads.emplace_back([&] {
        execution_guard reader;
        entered.fetch_add(1);
        while (entered.load() != 4) std::this_thread::yield();
        execution_guard nested;
    });
    for (auto& thread : threads) thread.join();
    require(entered.load() == 4, "ordinary reads were serialized");
}
static void writers_and_fibers() {
    auto database = db::create_mdb(4);
    require(database->open(""), "gate DB open");
    require(database->set("first", "0") && database->set("second", "0"),
        "gate DB seed");
    std::atomic<unsigned> generation{0};
    std::vector<std::thread> threads;
    for (unsigned thread = 0; thread < 4; ++thread)
        threads.emplace_back([&, thread] {
            for (unsigned fiber = 0; fiber < 4; ++fiber) go[&, thread, fiber] {
                for (unsigned i = 0; i < 40; ++i) {
                    if ((thread + fiber) % 3 == 0) {
                        execution_guard writer(true);
                        const auto value = std::to_string(
                            generation.fetch_add(1) + 1);
                        require(database->set("first", value), "gate first");
                        acl::fiber::yield();
                        require(database->set("second", value), "gate second");
                        execution_guard nested;
                    } else {
                        execution_guard reader;
                        std::string first, second;
                        require(database->get("first", first), "gate read");
                        acl::fiber::yield();
                        require(database->get("second", second) &&
                            first == second, "reader observed half an EXEC");
                    }
                }
            };
            acl::fiber::schedule();
        });
    for (auto& thread : threads) thread.join();
    require(generation.load() > 100, "gate contention fixture did no work");
}
static void frozen_time() {
    long long snapshot, micros;
    {
        execution_guard writer(true);
        snapshot = unix_time_ms(); micros = execution_time_us();
        require(micros / 1000 == snapshot, "clock snapshots disagree");
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
        require(unix_time_ms() == snapshot && execution_time_us() == micros,
            "EXEC clocks were not frozen");
        execution_guard reader;
        require(unix_time_ms() == snapshot, "nested read lost EXEC time");
    }
    require(unix_time_ms() > snapshot && execution_time_us() > micros,
        "clock stayed frozen after EXEC");
}
static void suspended() {
    bool rejected = false;
    try { execution_pause invalid; }
    catch (const std::logic_error&) { rejected = true; }
    require(rejected, "pause accepted missing admission");
    {
        execution_guard writer(true);
        rejected = false;
        try { execution_pause invalid; }
        catch (const std::logic_error&) { rejected = true; }
        require(rejected, "pause released an EXEC writer");
    }
    std::atomic<bool> waiting{false}, changed{false};
    std::thread writer([&] {
        while (!waiting.load()) std::this_thread::yield();
        execution_guard exclusive(true);
        changed.store(true);
    });
    {
        execution_guard reader, nested;
        {
            execution_pause pause;
            waiting.store(true);
            while (!changed.load()) std::this_thread::yield();
        }
        rejected = false;
        try { execution_guard invalid(true); }
        catch (const std::logic_error&) { rejected = true; }
        require(rejected, "pause lost nested shared admission");
        try {
            execution_pause pause;
            throw std::runtime_error("wait unwind");
        } catch (const std::runtime_error&) {}
        rejected = false;
        try { execution_guard invalid(true); }
        catch (const std::logic_error&) { rejected = true; }
        require(rejected, "wait unwind failed to restore admission");
    }
    writer.join();
    execution_guard writer_after(true);
}
static void suspended_fibers() {
    std::atomic<unsigned> waiting{0}, resumed{0};
    for (unsigned i = 0; i < 8; ++i) go[&] {
        execution_guard reader, nested;
        {
            execution_pause pause;
            waiting.fetch_add(1);
            while (waiting.load() != 9) acl::fiber::yield();
        }
        resumed.fetch_add(1);
    };
    go[&] {
        while (waiting.load() != 8) acl::fiber::yield();
        execution_guard writer(true);
        waiting.fetch_add(1);
        // Shared resumptions must wait until this exclusive actor leaves.
        for (unsigned i = 0; i < 16; ++i) acl::fiber::yield();
        require(!resumed.load(), "paused fiber bypassed active EXEC");
    };
    acl::fiber::schedule();
    require(resumed.load() == 8, "paused fibers failed to resume");
}
static void admission_across_actor_switches() {
    std::atomic<unsigned> completed{0};
    for (unsigned actor = 0; actor < 8; ++actor) go[&, actor] {
        const bool replica = actor % 2 == 0;
        replica_read_guard replica_scope(replica);
        for (unsigned round = 0; round < 20; ++round) {
            execution_guard outer;
            {
                execution_guard inner;
                execution_pause pause;
                acl::fiber::yield();
                require(execution_replica_read() == replica,
                    "paused actor inherited replica state");
            }
            acl::fiber::yield();
            require(execution_replica_read() == replica,
                "guard release crossed actor state");
        }
        completed.fetch_add(1);
    };
    go[&] {
        for (unsigned round = 0; round < 20; ++round) {
            execution_guard writer(true);
            const auto time = execution_time_us();
            acl::fiber::yield();
            execution_guard nested;
            require(execution_time_us() == time && !execution_replica_read(),
                "exclusive handoff lost actor-local state");
        }
    };
    acl::fiber::schedule();
    require(completed.load() == 8, "actors failed to release cached scopes");
    require(!execution_replica_read(), "replica scope escaped scheduler");
    execution_guard after(true);
}
static void clocks_across_actors() {
    std::atomic<bool> started{false}, thread_done{false};
    bool actor_done = false;
    long long snapshot = 0;
    auto ordinary_clock = [] {
        const auto ms = execution_time_ms(), us = execution_time_us();
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
        require(execution_time_ms() > ms && execution_time_us() > us,
            "ordinary actor inherited an EXEC clock");
    };
    std::thread outside([&] {
        while (!started.load()) std::this_thread::yield();
        ordinary_clock();
        thread_done.store(true);
    });
    go[&] {
        execution_guard writer(true);
        snapshot = execution_time_us();
        started.store(true);
        while (!actor_done || !thread_done.load()) {
            acl::fiber::yield();
            require(execution_time_us() == snapshot &&
                execution_time_ms() == snapshot / 1000,
                "yielded EXEC lost its frozen clock");
        }
    };
    go[&] {
        while (!started.load()) acl::fiber::yield();
        ordinary_clock();
        actor_done = true;
    };
    acl::fiber::schedule();
    outside.join();
    require(execution_time_us() > snapshot,
        "clock presence outlived exclusive admission");
}
int main() {
    acl::acl_cpp_init();
    nesting(); parallel_readers(); writers_and_fibers(); frozen_time();
    suspended(); suspended_fibers();
    admission_across_actor_switches();
    clocks_across_actors();
    // Exercise reuse of per-thread specific keys across scheduler runs.
    for (unsigned i = 0; i < 3; ++i) {
        go[] { nesting(); };
        acl::fiber::schedule();
    }
    std::cout << "execution gate reentrancy/thread/fiber checks passed\n";
}
