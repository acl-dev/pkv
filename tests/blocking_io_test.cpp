#include "stdafx.h"
#include "common/connection_input.h"
#include "common/blocked_client.h"
#include <sys/socket.h>
#include <fcntl.h>
#include <cstdlib>
#include <iostream>
#include <new>
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
static void pair(acl::socket_stream& first, acl::socket_stream& second) {
    int fd[2];
    require(!::socketpair(AF_UNIX, SOCK_STREAM, 0, fd), "input socketpair");
    for (const auto socket : fd)
        require(::fcntl(socket, F_SETFL, ::fcntl(socket, F_GETFL) |
            O_NONBLOCK) == 0, "nonblocking input socket");
    require(first.open(fd[0]) && second.open(fd[1]), "input streams");
}
static void chunks() {
    acl::socket_stream first, second;
    const std::string wire = std::string(40000, '\0') + std::string(30000, 'x');
    std::string copied;
    size_t received = 0, closed = 0;
    bool opened = false;
    go[&] {
        pair(first, second); opened = true;
        connection_input input(first, [&](size_t n) { received += n; },
            [&] { ++closed; });
        input.start();
        char buffer[37];
        for (;;) {
            const auto count = input.read(buffer, sizeof(buffer));
            if (count <= 0) break;
            copied.append(buffer, static_cast<size_t>(count));
            require(input.owned() >= input.pending(), "queue accounting");
        }
        input.stop(); input.stop();
        require(!input.pending() && closed == 1, "EOF/stop lifecycle");
        first.close();
    };
    go[&] {
        while (!opened) acl::fiber::delay(1);
        size_t offset = 0;
        while (offset < wire.size()) {
            const auto size = std::min<size_t>(7919, wire.size() - offset);
            const auto count = second.write(wire.data() + offset, size);
            require(count > 0, "input producer write");
            offset += static_cast<size_t>(count);
            acl::fiber::yield();
        }
        second.close();
    };
    acl::fiber::schedule();
    require(copied == wire && received == wire.size(), "input bytes changed");
}
static void bounded() {
    acl::socket_stream first, second;
    bool closed = false;
    size_t received = 0;
    bool opened = false;
    go[&] {
        pair(first, second); opened = true;
        connection_input input(first, [&](size_t n) { received += n; },
            [&] { closed = true; }, 128);
        input.start();
        while (!closed) acl::fiber::delay(1);
        char buffer[4];
        require(!input.read(buffer, sizeof(buffer)) && !input.pending() &&
            received > 128, "input quota did not close the producer");
        input.stop(); first.close(); second.close();
    };
    go[&] {
        while (!opened) acl::fiber::delay(1);
        const std::string data(1024, 'q');
        second.write(data.data(), data.size());
    };
    acl::fiber::schedule();
}
static void startup_failures() {
    unsigned failed = 0, succeeded = 0;
    for (long allowance = 0; allowance < 32; ++allowance) {
        acl::socket_stream first, second;
        go[&] {
            pair(first, second);
            unsigned closed = 0;
            try {
                budget = allowance;
                connection_input input(first, [](size_t) {},
                    [&] { ++closed; });
                input.start();
                budget = -1;
                for (unsigned i = 0; i < 3; ++i) acl::fiber::yield();
                input.stop(); ++succeeded;
                require(closed == 1, "reader stop callback count");
            } catch (const std::bad_alloc&) { budget = -1; ++failed; }
            first.close(); second.close();
        };
        acl::fiber::schedule();
    }
    require(failed && succeeded, "reader allocation sweep incomplete");
    std::cout << "Reader startup budgets: failed=" << failed << " succeeded="
        << succeeded << '\n';
}
static void stop_before_start() {
    acl::socket_stream first, second;
    unsigned closed = 0;
    go[&] {
        pair(first, second);
        connection_input input(first, [](size_t) {}, [&] { ++closed; });
        input.start(); input.stop();
        char byte;
        require(input.closed() && !input.read(&byte, 1) && closed == 1,
            "reader stopped before first scheduling remained alive");
        first.close(); second.close();
    };
    acl::fiber::schedule();
}
static void controls() {
    auto queue = std::make_shared<key_wait_registry>(true);
    auto token = queue->subscribe({"key"});
    blocked_client timeout(token);
    require(timeout.cancel(false) && !timeout.cancel(true) &&
        timeout.status() == blocked_client::state::timeout &&
        !queue->subscribers(), "cancel failed to detach waiting membership");
    key_wait_event event;
    require(!token->wait(0, event), "cancelled token still waited");
    auto another = queue->subscribe({"key"});
    blocked_client running(another);
    require(running.begin() && !running.cancel(true),
        "CLIENT UNBLOCK cancelled an executing operation");
    running.disconnect();
    require(!running.park() && !queue->subscribers(), "disconnect reparked");
    auto retained = queue->subscribe({"retained"});
    std::weak_ptr<key_wait_subscription> weak(retained);
    blocked_client observer(retained);
    observer.finish(); retained.reset();
    require(weak.expired(), "finished control retained FIFO membership");
}
int main() {
    acl::acl_cpp_init();
    chunks();
    std::cout << "Input EOF and fragmented chunks passed" << std::endl;
    bounded(); stop_before_start(); startup_failures(); controls();
    std::cout << "Blocking input chunks, quotas, startup and controls passed\n";
}
