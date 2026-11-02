#include "stdafx.h"
#include "common/connection_output.h"
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <new>
#include <thread>
#include <sys/socket.h>
#include <fcntl.h>

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
static connection_output::frame frame(const std::string& value) {
    return std::make_shared<const std::string>(value);
}
static void barriers() {
    go[] {
        std::string wire;
        unsigned closed = 0, replies = 0, pushes = 0;
        connection_output out([&](const char* data, size_t size) {
            wire.append(data, size); return size;
        }, [&](size_t, bool reply) { reply ? ++replies : ++pushes; },
            [&] { ++closed; });
        const auto baseline = out.owned();
        out.start();
        const auto first = out.reserve_reply();
        require(first && out.reserve_reply() == first, "nested reply ticket");
        require(out.append(frame("external-1")), "first external push");
        require(out.append(frame("self-1"), true), "self push");
        acl::fiber::yield();
        require(wire.empty(), "unfilled reply leaked a later push");
        budget = 0;
        require(out.capture_external(), "capture needs no allocation");
        budget = -1;
        require(out.append(frame("external-2")), "second external push");
        require(out.fulfil_reply(first, frame("reply-1")), "fulfil first");
        require(out.wait(first), "wait first");
        require(wire == "external-1reply-1", "reply barrier order");
        const auto second = out.reserve_reply();
        require(second > first && out.capture_external(), "second barrier");
        require(out.fulfil_reply(second, frame("reply-2")), "fulfil second");
        require(out.wait(second), "wait second");
        require(wire == "external-1reply-1self-1external-2reply-2",
            "push FIFO changed across reply barriers");
        require(!out.pending() && !out.frames() && out.owned() == baseline,
            "drained output retained memory");
        require(replies == 2 && pushes == 3, "reply versus push statistics");
        require(!out.fulfil_reply(first, frame("stale")) && !out.wait(99),
            "stale or future ticket accepted");
        out.stop(); out.stop();
        require(closed == 1 && out.closed(), "close callback repeated");
        require(out.wait(second) && !out.reserve_reply() &&
            !out.append(frame("late")), "closed output accepted work");
    };
    acl::fiber::schedule();
}
static void prefixes() {
    go[] {
        std::string wire;
        connection_output out([&](const char* data, size_t size) {
            wire.append(data, size); return size;
        }, [](size_t, bool) {}, [] {});
        out.start();
        const auto reply = out.reserve_reply();
        out.append(frame("a")); out.append(frame("s"), true);
        out.capture_external();
        out.append(frame("b")); out.capture_external();
        out.fulfil_reply(reply, frame("r"));
        require(out.wait(reply) && wire == "abr", "repeated capture order");
        const auto tail = out.reserve_reply();
        out.fulfil_reply(tail, frame("t"));
        require(out.wait(tail) && wire == "abrst", "self deferred after reply");
        out.stop();
    };
    acl::fiber::schedule();
}
static void bounds() {
    go[] {
        unsigned closed = 0;
        connection_output out([](const char*, size_t n) { return n; },
            [](size_t, bool) {}, [&] { ++closed; }, 4096);
        const auto baseline = out.owned();
        out.start();
        require(out.reserve_reply() && out.append(frame("")), "bounded seed");
        const auto oversized = frame(std::string(4096, 'x'));
        require(!out.append(oversized) && out.closed() && closed == 1,
            "payload and node quota not enforced");
        require(!out.frames() && !out.pending() && out.owned() == baseline,
            "quota failure retained pending frames");
        out.stop();
        connection_output zero([](const char*, size_t n) { return n; },
            [](size_t, bool) {}, [] {}, 0);
        require(!zero.reserve_reply() && zero.closed(), "zero node quota");
        connection_output empty([](const char*, size_t n) { return n; },
            [](size_t, bool) {}, [] {}, 4096);
        size_t accepted = 0;
        const auto tiny = frame("");
        while (empty.append(tiny)) ++accepted;
        require(accepted > 1 && accepted < 100 && !empty.frames(),
            "empty shared frames evaded heap quota");
        connection_output reserved([](const char*, size_t n) { return n; },
            [](size_t, bool) {}, [] {}, 4096);
        const auto id = reserved.reserve_reply();
        require(id && !reserved.fulfil_reply(id, oversized) &&
            reserved.closed() && !reserved.frames(), "reply quota bypass");
        connection_output capacity([](const char*, size_t n) { return n; },
            [](size_t, bool) {}, [] {}, 4096);
        auto retained = std::make_shared<std::string>("x");
        retained->reserve(8192);
        require(!capacity.append(retained) && capacity.closed(),
            "retained string capacity bypassed heap quota");
        connection_output cleanup([](const char*, size_t n) { return n; },
            [](size_t, bool) {}, [] {});
        const auto base = cleanup.owned();
        cleanup.reserve_reply(); cleanup.append(tiny);
        cleanup.capture_external(); cleanup.append(tiny, true);
        budget = 0;
        cleanup.stop();
        budget = -1;
        require(cleanup.owned() == base && !cleanup.frames(),
            "unfilled reply shutdown allocated or leaked prefixes");
    };
    acl::fiber::schedule();
}
static void partial() {
    go[] {
        unsigned calls = 0, closed = 0;
        size_t sent = 0;
        connection_output out([&](const char*, size_t n) {
            ++calls; return n - 1;
        }, [&](size_t n, bool reply) {
            require(reply, "partial reply lost classification"); sent += n;
        }, [&] { ++closed; });
        out.start();
        const auto id = out.reserve_reply();
        out.fulfil_reply(id, frame("broken"));
        out.append(frame("must-not-send"));
        require(!out.wait(id) && out.closed(),
            "partial write reported success");
        out.stop();
        require(calls == 1 && sent == 5 && closed == 1 && !out.frames(),
            "partial write accounting or cleanup");
    };
    acl::fiber::schedule();
}
static void large() {
    go[] {
        size_t sent = 0, largest = 0, calls = 0;
        connection_output out([&](const char*, size_t n) {
            sent += n; largest = std::max(largest, n); ++calls; return n;
        }, [](size_t, bool) {}, [] {}, 64ULL * 1024 * 1024);
        out.start();
        const auto id = out.reserve_reply();
        const size_t length = 17ULL * 1024 * 1024;
        out.fulfil_reply(id, frame(std::string(length, '\0')));
        require(out.wait(id) && sent == length && calls == 2 &&
            largest == 16ULL * 1024 * 1024, "bounded write chunks");
        out.stop();
    };
    acl::fiber::schedule();
}
static void concurrent() {
    go[] {
        std::string wire;
        connection_output out([&](const char* data, size_t n) {
            wire.append(data, n); return n;
        }, [](size_t, bool) {}, [] {}, 8ULL * 1024 * 1024);
        out.start();
        const auto barrier = out.reserve_reply();
        std::atomic<unsigned> done{0};
        std::vector<std::thread> producers;
        for (unsigned t = 0; t < 4; ++t) producers.emplace_back([&, t] {
            for (unsigned i = 0; i < 1000; ++i) {
                const auto data = std::to_string(t) + ":" +
                    std::to_string(i) + "\n";
                require(out.append(frame(data)), "cross-thread enqueue");
            }
            ++done;
        });
        while (done.load() != 4) acl::fiber::delay(1);
        for (auto& thread : producers) thread.join();
        require(wire.empty() && out.frames() == 4001,
            "cross-thread pushes crossed unfilled barrier");
        require(out.capture_external(), "concurrent capture");
        out.fulfil_reply(barrier, frame("done"));
        require(out.wait(barrier), "concurrent completion");
        size_t position = 0;
        unsigned next[4] = {};
        for (unsigned i = 0; i < 4000; ++i) {
            const auto end = wire.find('\n', position);
            require(end != std::string::npos, "missing concurrent frame");
            const auto t = static_cast<unsigned>(wire[position] - '0');
            require(t < 4 && wire.substr(position, end - position) ==
                std::to_string(t) + ":" + std::to_string(next[t]++),
                "producer FIFO or frame atomicity violated");
            position = end + 1;
        }
        require(wire.substr(position) == "done" && !out.frames(),
            "reply completed before captured pushes");
        out.stop();
    };
    acl::fiber::schedule();
}
static void failures() {
    unsigned failed = 0, succeeded = 0;
    for (long allowance = 0; allowance < 32; ++allowance) {
        go[&] {
            unsigned closed = 0;
            try {
                budget = allowance;
                connection_output out([](const char*, size_t n) { return n; },
                    [](size_t, bool) {}, [&] { ++closed; });
                out.start(); budget = -1;
                out.stop(); ++succeeded;
                require(closed == 1, "startup cleanup callback");
            } catch (const std::bad_alloc&) { budget = -1; ++failed; }
        };
        acl::fiber::schedule();
    }
    require(failed && succeeded, "startup allocation budgets incomplete");
    go[] {
        const auto data = frame("payload");
        connection_output out([](const char*, size_t n) { return n; },
            [](size_t, bool) {}, [] {});
        const auto base = out.owned();
        out.reserve_reply();
        budget = 0;
        const bool accepted = out.append(data);
        budget = -1;
        require(!accepted && out.closed() && !out.frames() &&
            out.owned() == base, "enqueue OOM failed to close and clean");
        connection_output reserve([](const char*, size_t n) { return n; },
            [](size_t, bool) {}, [] {});
        budget = 0;
        const auto id = reserve.reserve_reply();
        budget = -1;
        require(!id && reserve.closed(), "reserve OOM escaped");
    };
    acl::fiber::schedule();
    std::cout << "Output startup budgets: failed=" << failed <<
        " succeeded=" << succeeded << '\n';
}
static void sockets(acl::socket_stream& first, acl::socket_stream& second,
    int& descriptor) {
    int fd[2];
    require(!::socketpair(AF_UNIX, SOCK_STREAM, 0, fd), "output socketpair");
    for (const auto socket : fd)
        require(::fcntl(socket, F_SETFL, ::fcntl(socket, F_GETFL) |
            O_NONBLOCK) == 0, "nonblocking output socket");
    const int small = 1024;
    require(!::setsockopt(fd[0], SOL_SOCKET, SO_SNDBUF, &small, sizeof(small)),
        "small socket send buffer");
    descriptor = fd[0];
    require(first.open(fd[0]) && second.open(fd[1]), "output streams");
}
static void socket_bytes() {
    acl::socket_stream first, second;
    std::string received;
    const auto payload = frame(std::string(65536, '\0') + "binary-tail");
    bool opened = false;
    int descriptor = -1;
    go[&] {
        sockets(first, second, descriptor); opened = true;
        connection_output out([&](const char* data, size_t size) {
            const auto sent = first.write(data, size);
            return sent > 0 ? static_cast<size_t>(sent) : 0;
        }, [](size_t, bool) {}, [&] { ::shutdown(descriptor, SHUT_RDWR); });
        out.start();
        const auto id = out.reserve_reply();
        out.fulfil_reply(id, payload);
        require(out.wait(id) && !out.pending(), "socket reply completion");
        out.stop(); first.close();
    };
    go[&] {
        while (!opened) acl::fiber::delay(1);
        char data[4096];
        int count;
        while ((count = second.read(data, sizeof(data), false)) > 0) {
            received.append(data, static_cast<size_t>(count));
            acl::fiber::yield();
        }
        second.close();
    };
    acl::fiber::schedule();
    require(received == *payload, "socket binary payload corruption");
}
static void slow_socket() {
    go[] {
        acl::socket_stream first, second;
        int descriptor = -1;
        sockets(first, second, descriptor);
        unsigned closed = 0;
        bool writing = false;
        connection_output out([&](const char* data, size_t size) {
            writing = true;
            const auto sent = first.write(data, size);
            return sent > 0 ? static_cast<size_t>(sent) : 0;
        }, [](size_t, bool) {}, [&] {
            ++closed; ::shutdown(descriptor, SHUT_RDWR);
        }, 2ULL * 1024 * 1024);
        out.start();
        const auto base = out.owned();
        const auto payload = frame(std::string(1024 * 1024, 's'));
        require(out.append(payload), "slow socket seed");
        while (!writing) acl::fiber::yield();
        require(out.pending() == payload->size() && out.frames() == 1,
            "in-flight payload excluded from quota");
        require(!out.append(payload) && out.closed() && closed == 1,
            "slow socket did not close at heap quota");
        out.stop();
        require(!out.frames() && !out.pending() && out.owned() == base,
            "blocked writer stop retained in-flight payload");
        first.close(); second.close();
    };
    acl::fiber::schedule();
}
static void concurrent_drain() {
    go[] {
        std::atomic<unsigned> done{0};
        size_t deliveries = 0;
        unsigned next[4] = {};
        connection_output out([&](const char* data, size_t size) {
            if (size) {
                const auto t = static_cast<unsigned>(data[0] - '0');
                require(t < 4 && std::string(data, size) ==
                    std::to_string(t) + ":" + std::to_string(next[t]++),
                    "concurrent writer producer FIFO changed");
                ++deliveries;
            }
            return size;
        }, [](size_t, bool) {}, [] {}, 8ULL * 1024 * 1024);
        out.start();
        std::vector<std::thread> producers;
        for (unsigned t = 0; t < 4; ++t) producers.emplace_back([&, t] {
            for (unsigned i = 0; i < 4096; ++i)
                require(out.append(frame(std::to_string(t) + ":" +
                    std::to_string(i))), "parallel drain append");
            ++done;
        });
        while (done.load() != 4) acl::fiber::delay(1);
        for (auto& thread : producers) thread.join();
        const auto id = out.reserve_reply();
        out.fulfil_reply(id, frame(""));
        require(out.wait(id) && deliveries == 16384 && !out.frames(),
            "parallel drain dropped frames");
        out.stop();
    };
    acl::fiber::schedule();
}
static void send_failure() {
    go[] {
        unsigned closed = 0, calls = 0;
        size_t accounted = 0;
        connection_output out([&](const char*, size_t size) {
            if (++calls == 2) throw std::bad_alloc();
            return size;
        }, [&](size_t size, bool) { accounted += size; },
            [&] { ++closed; }, 64ULL * 1024 * 1024);
        out.start();
        const auto id = out.reserve_reply();
        out.fulfil_reply(id, frame(std::string(17ULL * 1024 * 1024, 'x')));
        require(!out.wait(id) && closed == 1 &&
            accounted == 16ULL * 1024 * 1024,
            "send allocation failure lost previously written chunks");
        out.stop();
        require(!out.pending() && !out.frames(), "failed send retained work");
    };
    acl::fiber::schedule();
}
static void shutdown_race() {
    for (unsigned round = 0; round < 32; ++round) {
        go[] {
            std::atomic<bool> done{false};
            std::atomic<unsigned> closes{0};
            connection_output out([](const char*, size_t n) { return n; },
                [](size_t, bool) {}, [&] { ++closes; });
            out.start();
            const auto id = out.reserve_reply();
            out.append(frame("old-external")); out.capture_external();
            out.append(frame("self"), true);
            std::thread producer([&] {
                const auto data = frame("new-external");
                while (!done.load() && out.append(data))
                    std::this_thread::yield();
                done = true;
            });
            acl::fiber::yield();
            out.stop(); done = true; producer.join();
            require(!out.wait(id) && closes == 1 && !out.pending() &&
                !out.frames(), "concurrent shutdown retained frames");
        };
        acl::fiber::schedule();
    }
}
static void changing_limits() {
    unsigned closed = 0;
    connection_output out([](const char*, size_t n) { return n; },
        [](size_t, bool) {}, [&] { ++closed; }, 4096);
    const auto baseline = out.owned();
    require(out.append(frame(std::string(1024, 'a'))) && out.limit(8192) &&
        out.append(frame(std::string(4096, 'b'))), "raised output quota");
    budget = 0;
    const bool accepted = out.limit(1024);
    budget = -1;
    require(!accepted && out.closed() && closed == 1 && !out.pending() &&
        !out.frames() && out.owned() == baseline && !out.limit(8192),
        "lowered output quota failed to close and clean");
}
int main() {
    acl::acl_cpp_init();
    barriers(); prefixes(); bounds(); partial(); large(); concurrent();
    failures(); socket_bytes(); slow_socket(); concurrent_drain();
    send_failure(); shutdown_race(); changing_limits();
    std::cout << "Output barriers, quotas, writes, FIFO and OOM passed\n";
}
