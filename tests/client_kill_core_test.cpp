#include "stdafx.h"
#include "action/redis_handler.h"
#include "action/redis_server.h"
#include "action/redis_service.h"
#include "coder/redis_ocache.h"
#include "common/execution_gate.h"
#include "db/database_pool.h"
#include "db/mdb/mdb.h"
#include <cstdlib>
#include <future>
#include <iostream>
#include <new>
#include <thread>
#include <sys/socket.h>
#include <unistd.h>

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
static void require(bool ok, const char* reason) {
    if (!ok) { std::cerr << reason << '\n'; std::exit(1); }
}
struct client {
    redis_ocache cache{64};
    redis_coder parser{cache};
    acl::socket_stream writer, reader;
    std::unique_ptr<redis_handler> handler;
    client(redis_service& service, std::shared_ptr<database_pool> pool) {
        int sockets[2];
        require(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0,
            "socketpair");
        require(writer.open(sockets[0]) && reader.open(sockets[1]), "streams");
        handler.reset(new redis_handler(service, pool, parser, writer));
    }
    void prepare(const std::vector<std::string>& args) {
        std::string wire = '*' + std::to_string(args.size()) + "\r\n";
        for (const auto& arg : args)
            wire += '$' + std::to_string(arg.size()) + "\r\n" + arg + "\r\n";
        size_t remaining = wire.size();
        parser.update(wire.data(), remaining);
        require(!remaining && !parser.get_curr()->failed(), "parse");
    }
};
static std::shared_ptr<database_pool> database() {
    auto zero = std::make_shared<mdb>(4);
    require(zero->open(""), "database open");
    return std::make_shared<database_pool>(zero, 1,
        [](size_t) { return shared_db(); });
}
static void admission() {
    redis_service service;
    auto pool = database();
    client admin(service, pool), victim(service, pool);
    const auto id = victim.handler->client().id();
    admin.prepare({"CLIENT", "KILL", "ID", std::to_string(id)});
    std::promise<bool> promise;
    auto done = promise.get_future();
    std::thread worker;
    {
        execution_guard exclusive(true);
        worker = std::thread([&] {
            try { promise.set_value(admin.handler->handle()); }
            catch (...) { promise.set_exception(std::current_exception()); }
        });
        require(done.wait_for(std::chrono::seconds(2)) ==
            std::future_status::ready, "KILL waited on KV execution gate");
        require(done.get(), "KILL handler failed");
        require(service.clients().find(id)->status() ==
            client_connection::state::immediate, "target not stopped");
    }
    worker.join();
    char reply[4];
    require(::recv(admin.reader.sock_handle(), reply, sizeof(reply),
        MSG_DONTWAIT) == 4 && std::string(reply, 4) == ":1\r\n",
        "KILL reply");
}
static void allocation_failures() {
    size_t failed = 0, passed = 0;
    for (bool legacy : {false, true}) {
        for (long available = 0; available < 16; ++available) {
            redis_service service;
            auto pool = database();
            client admin(service, pool), one(service, pool), two(service, pool);
            if (legacy) admin.prepare({"CLIENT", "KILL", "?:0"});
            else admin.prepare({"CLIENT", "KILL", "USER", "default"});
            auto first = service.clients().find(one.handler->client().id());
            auto second = service.clients().find(two.handler->client().id());
            redis_coder response(admin.cache);
            auto& reply = response.create_object();
            redis_server action(*admin.handler, *admin.parser.get_objects()[0]);
            bool oom = false;
            budget = available;
            try { action.client_kill(reply); }
            catch (const std::bad_alloc&) { oom = true; }
            budget = -1;
            if (oom) {
                ++failed;
                require(first->status() == client_connection::state::open &&
                    second->status() == client_connection::state::open,
                    "allocation failure after partial shutdown");
            } else {
                ++passed;
                std::string wire;
                require(reply.to_resp(wire) && wire ==
                    (legacy ? "+OK\r\n" : ":2\r\n"), "OOM success reply");
                require(first->status() ==
                    client_connection::state::immediate && second->status() ==
                    client_connection::state::immediate, "OOM success kill");
            }
        }
    }
    require(failed >= 2 && passed, "KILL allocation hooks did not run");
}
int main() {
    admission(); allocation_failures();
    std::cout << "CLIENT KILL admission and pre-shutdown OOM checks passed\n";
}
