#include "stdafx.h"
#include "action/redis_handler.h"
#include "action/redis_service.h"
#include "coder/redis_ocache.h"
#include "db/database_pool.h"
#include "db/mdb/mdb.h"
#include <cstdlib>
#include <iostream>
#include <sys/socket.h>
#include <unistd.h>

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
    void prepare(const std::string& wire) {
        parser.clear();
        size_t remaining = wire.size();
        parser.update(wire.data(), remaining);
        require(!remaining && !parser.get_curr()->failed(), "parse");
    }
};
int main() {
    redis_service service;
    auto db = std::make_shared<mdb>(4);
    require(db->open(""), "database open");
    auto pool = std::make_shared<database_pool>(db, 1,
        [](size_t) { return shared_db(); });
    client_registry::connection retained;
    for (bool deferred : {false, true}) {
        client connection(service, pool);
        const auto entries = service.clients().snapshot();
        require(entries.size() == 1 && entries[0]->id() ==
            connection.handler->client().id(), "handler registration");
        retained = entries[0];
        connection.prepare("*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$1\r\nv\r\n");
        require(retained->request_stop(deferred) &&
            !connection.handler->handle(), "stopped handler ran pipeline");
        std::string value;
        require(!db->get("k", value), "stopped command changed database");
        connection.handler.reset();
        require(service.clients().snapshot().empty() && retained->status() ==
            client_connection::state::retired, "handler retirement");
        int type = 0; socklen_t length = sizeof(type);
        require(::getsockopt(connection.writer.sock_handle(), SOL_SOCKET,
            SO_TYPE, &type, &length) == 0, "handler closed socket owner");
    }
    {
        client first(service, pool), second(service, pool);
        auto entries = service.clients().snapshot();
        require(entries.size() == 2, "simultaneous handler registration");
        first.prepare("*1\r\n$4\r\nPING\r\n");
        require(first.handler->handle(), "live handler rejected command");
        char reply[7];
        require(::recv(first.reader.sock_handle(), reply, sizeof(reply), 0)
            == 7 && std::string(reply, 7) == "+PONG\r\n", "live reply");
        first.handler.reset();
        require(service.clients().snapshot().size() == 1 &&
            service.clients().find(second.handler->client().id()),
            "retirement removed another handler");
    }
    require(service.clients().snapshot().empty(), "handler lifetime leak");
    std::cout << "Handler registration, stop and owner-close passed\n";
}
