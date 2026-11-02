#include "common/client_registry.h"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <new>
#include <stdexcept>
#include <thread>
#ifndef _WIN32
#include <cerrno>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

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
using state = client_connection::state;
static void require(bool ok, const char* reason) {
    if (!ok) { std::cerr << reason << '\n'; std::exit(1); }
}
static void membership() {
    client_registry registry, foreign;
    auto one = registry.add(1, 100);
    auto two = registry.add(65, 101);
    auto other = foreign.add(1, 102);
    require(!foreign.retire(one) && !registry.retire(other),
        "foreign ownership check");
    bool duplicate = false, invalid = false;
    try { registry.add(1, 103); }
    catch (const std::logic_error&) { duplicate = true; }
    try { registry.add(0, 103); }
    catch (const std::invalid_argument&) { invalid = true; }
    require(duplicate && invalid && registry.find(1) == one &&
        !registry.find(-1) && !registry.retire({}), "membership validation");
    const auto saved = registry.snapshot();
    require(saved.size() == 2 && saved[0] == one && saved[1] == two,
        "ordered membership snapshot");
    require(registry.retire(one) && !registry.retire(one) &&
        one->status() == state::retired && !one->request_stop() &&
        !registry.find(1) && registry.find(65) == two,
        "retirement with retained snapshot");
    auto replacement = registry.add(1, 104);
    require(!registry.retire(one) && registry.find(1) == replacement,
        "stale retirement removed replacement");
    client_registry::connection retained;
    {
        client_registry temporary;
        retained = temporary.add(2, 105);
    }
    require(retained->status() == state::retired &&
        !retained->request_stop(), "registry destruction invalidation");
}
static void allocation_failures() {
    size_t failures = 0, successes = 0;
    for (long available = 0; available < 8; ++available) {
        client_registry registry;
        client_registry::connection entry;
        budget = available;
        try { entry = registry.add(1, 100); }
        catch (const std::bad_alloc&) { ++failures; }
        budget = -1;
        if (entry) {
            ++successes;
            require(registry.retire(entry), "allocation success membership");
        }
        require(registry.snapshot().empty(), "failed insertion published");
    }
    require(failures >= 2 && successes, "insertion allocation coverage");
    client_registry registry;
    auto entry = registry.add(1, 100);
    bool failed = false;
    budget = 0;
    try { registry.snapshot(); }
    catch (const std::bad_alloc&) { failed = true; }
    budget = -1;
    require(failed && registry.find(1) == entry && registry.retire(entry),
        "failed snapshot retained locks or changed membership");
}
static void concurrent_membership() {
    client_registry registry;
    std::atomic<bool> start{false};
    std::atomic<unsigned> finished{0};
    std::vector<std::thread> workers;
    for (unsigned worker = 0; worker < 8; ++worker) {
        workers.emplace_back([&, worker] {
            while (!start.load()) std::this_thread::yield();
            for (unsigned iteration = 0; iteration < 2000; ++iteration) {
                const auto id = 1 + worker + 8LL * iteration;
                auto entry = registry.add(id, 100);
                require(registry.find(id) == entry, "concurrent lookup");
                require(registry.retire(entry), "concurrent retirement");
            }
            ++finished;
        });
    }
    start = true;
    size_t snapshots = 0;
    do {
        const auto snapshot = registry.snapshot();
        long long previous = 0;
        for (const auto& entry : snapshot) {
            require(entry->id() > previous, "snapshot duplicate or order");
            previous = entry->id();
        }
        ++snapshots;
    } while (finished.load() != 8);
    for (auto& worker : workers) worker.join();
    require(snapshots && registry.snapshot().empty(), "concurrent drain");
}
#ifndef _WIN32
static void socket_pair(int (&fds)[2]) {
    require(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0, "socketpair");
}
static void live_peer(int fd) {
    char ch = 0;
    const auto received = ::recv(fd, &ch, 1, MSG_DONTWAIT);
    require(received == -1 && (errno == EAGAIN || errno == EWOULDBLOCK),
        "live peer was shut down");
}
static void addresses() {
    for (bool ipv6 : {false, true}) {
        const int family = ipv6 ? AF_INET6 : AF_INET;
        const int listener = ::socket(family, SOCK_STREAM, 0);
        require(listener >= 0, "TCP listener");
        sockaddr_storage storage{};
        auto* address = reinterpret_cast<sockaddr*>(&storage);
        socklen_t size;
        if (ipv6) {
            auto& value = *reinterpret_cast<sockaddr_in6*>(&storage);
            value.sin6_family = AF_INET6; value.sin6_addr = in6addr_loopback;
            size = sizeof(value);
        } else {
            auto& value = *reinterpret_cast<sockaddr_in*>(&storage);
            value.sin_family = AF_INET;
            value.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            size = sizeof(value);
        }
        require(::bind(listener, address, size) == 0 &&
            ::listen(listener, 1) == 0 &&
            ::getsockname(listener, address, &size) == 0, "TCP bind/listen");
        const int sender = ::socket(family, SOCK_STREAM, 0);
        require(sender >= 0 && ::connect(sender, address, size) == 0,
            "TCP connect");
        const int accepted = ::accept(listener, nullptr, nullptr);
        require(accepted >= 0, "TCP accept");
        client_registry registry;
        auto server = registry.add(1, accepted);
        auto peer = registry.add(2, sender);
        const std::string prefix = ipv6 ? "[::1]:" : "127.0.0.1:";
        const auto now = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        require(server->address() == peer->local_address() &&
            server->local_address() == peer->address() &&
            server->address().find(prefix) == 0 &&
            server->local_address().find(prefix) == 0 &&
            server->age(now) >= 0 && server->age(now) <= 1,
            "immutable IPv4/IPv6 endpoint and age");
        registry.retire(server); registry.retire(peer);
        ::close(accepted); ::close(sender); ::close(listener);
    }
}
static void socket_lifecycle() {
    client_registry registry;
    int fds[2]; socket_pair(fds);
    auto entry = registry.add(1, fds[0]);
    require(entry->request_stop(true) && !entry->request_stop(true) &&
        entry->status() == state::after_reply, "deferred self-close");
    live_peer(fds[1]);
    char reply[2];
    require(::send(fds[0], "OK", 2, 0) == 2 &&
        ::recv(fds[1], reply, 2, 0) == 2 && reply[0] == 'O' &&
        reply[1] == 'K', "deferred close prevented final reply");
    require(!entry->request_stop() && entry->status() == state::immediate,
        "close escalation counted twice");
    char ch = 0;
    require(::recv(fds[1], &ch, 1, MSG_DONTWAIT) == 0,
        "shutdown did not wake peer");
    int type = 0; socklen_t length = sizeof(type);
    require(::getsockopt(fds[0], SOL_SOCKET, SO_TYPE, &type, &length) == 0,
        "management closed owner descriptor");
    registry.retire(entry); ::close(fds[0]); ::close(fds[1]);
    for (long long id = 2; id < 502; ++id) {
        socket_pair(fds);
        entry = registry.add(id, fds[0]);
        auto snapshot = registry.snapshot();
        const int reused = fds[0];
        require(registry.retire(entry), "retire before descriptor close");
        ::close(fds[0]); ::close(fds[1]);
        int fresh[2]; socket_pair(fresh);
        if (fresh[1] == reused) std::swap(fresh[0], fresh[1]);
        if (fresh[0] != reused) {
            require(::dup2(fresh[0], reused) == reused, "descriptor reuse");
            ::close(fresh[0]); fresh[0] = reused;
        }
        require(!snapshot[0]->request_stop(), "stale snapshot accepted stop");
        live_peer(fresh[1]);
        ::close(fresh[0]); ::close(fresh[1]);
    }
}
static void concurrent_stop() {
    client_registry registry;
    for (long long id = 1; id <= 300; ++id) {
        int fds[2]; socket_pair(fds);
        auto entry = registry.add(id, fds[0]);
        std::atomic<bool> start{false};
        std::thread stopper([&] {
            while (!start.load()) std::this_thread::yield();
            entry->request_stop();
        });
        start = true;
        require(registry.retire(entry), "stop/retire race membership");
        const int reused = fds[0];
        ::close(fds[0]);
        int fresh[2]; socket_pair(fresh);
        if (fresh[1] == reused) std::swap(fresh[0], fresh[1]);
        if (fresh[0] != reused) {
            require(::dup2(fresh[0], reused) == reused, "racing reuse");
            ::close(fresh[0]); fresh[0] = reused;
        }
        stopper.join();
        live_peer(fresh[1]);
        ::close(fresh[0]); ::close(fresh[1]);
        require(entry->status() == state::retired && !entry->request_stop(),
            "stop/retire race final state");
        ::close(fds[1]);
    }
}
#endif
int main() {
    membership(); allocation_failures(); concurrent_membership();
#ifndef _WIN32
    addresses(); socket_lifecycle(); concurrent_stop();
#endif
    std::cout << "Client registry lifecycle, OOM and concurrency passed\n";
}
