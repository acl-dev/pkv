#include "stdafx.h"
#include "action/redis_handler.h"
#include "action/redis_service.h"
#include "coder/redis_ocache.h"
#include "common/execution_gate.h"
#include "db/database_pool.h"
#include <sys/socket.h>
#include <fcntl.h>
#include <cstdlib>
#include <iostream>
#include <new>
static thread_local long budget = -1;
void* operator new(std::size_t size) {
    if (!budget) { budget = -1; throw std::bad_alloc(); }
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
static void sockets(acl::socket_stream& first, acl::socket_stream& second) {
    int fd[2];
    require(!::socketpair(AF_UNIX, SOCK_STREAM, 0, fd), "handler socketpair");
    for (const auto descriptor : fd)
        require(::fcntl(descriptor, F_SETFL, ::fcntl(descriptor, F_GETFL) |
            O_NONBLOCK) == 0, "nonblocking handler socket");
    require(first.open(fd[0]) && second.open(fd[1]), "handler streams");
}
static void receive(redis_handler& handler, const std::string& wire) {
    size_t size = wire.size();
    handler.receive(wire.data(), size, wire.size());
    require(!size, "handler input not consumed");
}
static void read_echo(acl::socket_stream& peer, const std::string& expected,
    bool& done, bool& correct) {
    char buffer[8192];
    size_t offset = 0;
    correct = true;
    while (offset < expected.size()) {
        const auto count = peer.read(buffer,
            std::min(sizeof(buffer), expected.size() - offset), false);
        if (count <= 0) { correct = false; break; }
        if (std::memcmp(buffer, expected.data() + offset,
            static_cast<size_t>(count))) correct = false;
        offset += static_cast<size_t>(count);
    }
    done = true;
}
static void attempt(long allowance, unsigned& closed, unsigned& subscribed,
    unsigned& recovered, const std::string& input,
    const std::string& expected) {
    go[&] {
        acl::socket_stream socket, peer;
        sockets(socket, peer);
        redis_service service;
        auto database = db::create_mdb(4);
        require(database->open(""), "handler database");
        auto databases = std::make_shared<database_pool>(database, 1,
            [](size_t) { return shared_db(); });
        redis_ocache cache(100);
        redis_coder parser(cache);
        {
            execution_guard warmed;
        }
        {
            redis_handler handler(service, databases, parser, socket);
            receive(handler, "*2\r\n$9\r\nSUBSCRIBE\r\n$4\r\nname\r\n");
            bool alive = false;
            try {
                budget = allowance;
                alive = handler.handle();
                budget = -1;
            } catch (const std::bad_alloc&) { budget = -1; }
            if (!alive) ++closed;
            else if (handler.subscribed()) ++subscribed;
            else {
                acl::string error;
                require(peer.gets(error) && error.size() && error[0] == '-',
                    "failed subscription reply");
                handler.clear_input(); receive(handler, input);
                bool done = false, correct = false;
                go[&] { read_echo(peer, expected, done, correct); };
                const auto usable = handler.handle();
                if (!usable) ::shutdown(socket.sock_handle(), SHUT_RDWR);
                while (!done) acl::fiber::delay(1);
                if (!usable || !correct)
                    std::cerr << "Normal reply after failed first SUBSCRIBE "
                        "closed at allocation budget " << allowance << '\n';
                require(usable && correct,
                    "failed subscription retained the PubSub output quota");
                ++recovered;
            }
        }
        socket.close(); peer.close();
        require(!service.pubsub().counts().total(),
            "handler failure retained subscriptions");
    };
    acl::fiber::schedule();
}
int main() {
    acl::acl_cpp_init();
    const std::string payload(33ULL * 1024 * 1024, 'x');
    const auto length = std::to_string(payload.size());
    const std::string input = "*2\r\n$4\r\nECHO\r\n$" + length + "\r\n" +
        payload + "\r\n";
    const std::string expected = "$" + length + "\r\n" + payload + "\r\n";
    unsigned closed = 0, subscribed = 0, recovered = 0;
    const char* selected = std::getenv("PKV_HANDLER_BUDGET");
    const long first = selected ? std::strtol(selected, nullptr, 10) : 0;
    const long end = selected ? first + 1 : 64;
    for (long allowance = first; allowance < end; ++allowance)
        attempt(allowance, closed, subscribed, recovered, input, expected);
    require(selected || (closed && subscribed && recovered &&
        closed + subscribed + recovered == 64), "handler fault sweep coverage");
    std::cout << "PubSub handler budgets: closed=" << closed <<
        " subscribed=" << subscribed << " recovered=" << recovered << '\n';
    std::cout << "Failed first subscriptions retain normal large replies\n";
}
