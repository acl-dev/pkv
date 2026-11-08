#include "stdafx.h"
#include "db/db.h"
#include "db/db_watcher.h"
#include "db/page_transaction.h"
#include "db/database_pool.h"
#include "slave/slave_watcher.h"
#include "slave/slave_client.h"
#include "rpc/rpc_reader.h"
#include "cluster/cluster_client.h"
#include <sys/socket.h>
#include <unistd.h>
#include <poll.h>
#include <chrono>
#include <thread>
#include <iostream>

using namespace pkv;

static void require(bool ok, const char* reason) {
    if (!ok) {
        std::cerr << reason << '\n';
        std::exit(1);
    }
}

struct streams {
    acl::shared_stream server = std::make_shared<acl::socket_stream>();
    acl::shared_stream peer = std::make_shared<acl::socket_stream>();
    streams() {
        int sockets[2];
        require(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0,
            "socketpair");
        require(server->open(sockets[0]) && peer->open(sockets[1]),
            "stream open");
        peer->set_rw_timeout(3);
    }
};

static shared_db memory(db_watchers* watchers = nullptr) {
    auto database = db::create_mdb(2);
    require(database->open("", watchers), "memory open");
    return database;
}

static void wait_drained(slave_client& client) {
    for (unsigned i = 0; i < 1000 && client.pending_messages(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    require(client.pending_messages() == 0, "subscriber did not drain READY");
}

static void handshake(streams& pair, unsigned version) {
    const std::string request = "PKV-REPL " + std::to_string(version) + "\r\n";
    require(pair.peer->write(request.data(), request.size()) ==
        static_cast<int>(request.size()), "send handshake");
    rpc_packet packet;
    require(rpc_reader::read_packet(pair.peer, packet) && !packet.page &&
        packet.message && packet.message->get_oper() == message_oper_ready &&
        packet.message->get_protocol() == version, "negotiated READY");
}

static bool write_pages(shared_db source, size_t size = 1000, char fill = 'x') {
    return source->transact_pages({"list"}, [&](page_transaction& view) {
        auto& root = *view.roots().find("list");
        root.value = "root0";
        root.action = db::update_action::write;
        return view.put("list", 1, std::string(size, fill));
    }) == db::batch_status::ok;
}

static void verify_page(shared_db destination, size_t size = 1000,
    char fill = 'x') {
    std::string root;
    require(destination->get("list", root) && root == "root0",
        "live root missing");
    require(destination->transact_pages({"list"}, [&](page_transaction& view) {
        std::string value;
        require(view.lookup("list", 1, value) == db::lookup_status::found &&
            value == std::string(size, fill), "live physical page missing");
        return true;
    }) == db::batch_status::ok, "live page read");
}

// 56 control bytes + (20 + 4 + 5) root + (20 + 4 + 8 + 1000) page.
static const size_t group_bytes = 1117;
static const size_t group_records = 4;

static void run_path(unsigned limit) {
    const bool publish_limit = limit == 2 || limit == 4;
    const bool fail = limit >= 1 && limit <= 4;
    const bool db_seven = limit == 5;
    const bool marker = limit == 6;
    const bool replace = limit == 7;
    const size_t page_bytes = replace ? 1500 : 1000;
    const size_t expected_bytes = group_bytes + page_bytes - 1000;
    const size_t capacity = marker ? expected_bytes + 128 : expected_bytes;
    const size_t records = marker ? group_records + 4 : group_records;
    const size_t byte_limit = limit == 1 || limit == 2 ? group_bytes - 1 :
        capacity;
    const size_t record_limit = limit == 3 || limit == 4 ? group_records - 1 :
        records;
    slave_watcher publisher(publish_limit ? byte_limit : capacity,
        publish_limit ? record_limit : records);
    db_watchers observers(db_seven ? 7 : 0);
    observers.add_watcher(&publisher);
    auto source = memory(&observers);
    if (replace) {
        require(write_pages(source, 2100, 'a') && write_pages(source, 1800, 'b'),
            "unobserved page replacement failed");
        verify_page(source, 1800, 'b');
        require(!publisher.pending_messages(), "queued pages without subscribers");
    }
    auto destination = memory();
    auto pool = std::make_shared<database_pool>(destination, 16,
        [](size_t) { return memory(); });
    auto selected = db_seven ? pool->get(7) : destination;
    cluster_client receiver("", destination, pool);
    streams pair, legacy, version_one;
    auto client = std::make_shared<slave_client>(pair.server,
        publish_limit ? capacity : byte_limit,
        publish_limit ? records : record_limit);
    auto old = std::make_shared<slave_client>(legacy.server);
    auto one = std::make_shared<slave_client>(version_one.server);
    publisher.add_client(client);
    publisher.add_client(old);
    publisher.add_client(one);
    go[client] { client->run(); };
    go[old] { old->run(); };
    go[one] { one->run(); };
    std::thread controller([&] {
        handshake(pair, 2);
        handshake(version_one, 1);
        wait_drained(*client);
        wait_drained(*one);
        require(receiver.apply(std::make_shared<kv_message>("", "",
            message_oper_ready, 0, 2)), "receiver READY");
        require(write_pages(source, page_bytes), "observed native page commit rejected");
        verify_page(source, page_bytes);
        if (publish_limit) {
            require(client->closed() && old->closed() && one->closed(),
                "publisher overflow retained subscribers");
        } else {
            require(publisher.pending_bytes() == expected_bytes &&
                publisher.pending_messages() == group_records,
                "publisher did not charge all transaction frames");
        }
        if (marker) {
            require(source->set("marker", "after"), "post-group marker");
        }
        publisher.stop();
        std::thread forwarder([&] { publisher.run(); });
        if (!fail) {
            for (size_t i = 0; i < group_records; ++i) {
                rpc_packet packet;
                require(rpc_reader::read_packet(pair.peer, packet) &&
                    packet.page && receiver.apply_packet(std::move(packet)),
                    "live packet read/apply");
                if (i + 1 != group_records) {
                    std::string root;
                    require(!selected->get("list", root),
                        "live prefix changed destination");
                }
            }
            verify_page(selected, page_bytes);
            if (db_seven) {
                std::string root;
                require(!destination->get("list", root), "DB seven leaked");
            }
            if (marker) {
                rpc_packet packet;
                require(rpc_reader::read_packet(pair.peer, packet) &&
                    !packet.page && receiver.apply_packet(std::move(packet)),
                    "normal record interleaved into page group");
                std::string value;
                require(selected->get("marker", value) && value == "after",
                    "post-group normal update missing");
            }
        } else {
            rpc_packet packet;
            require(!rpc_reader::read_packet(pair.peer, packet),
                "over-limit subscriber kept its stream");
            std::string root;
            require(!destination->get("list", root),
                "overflow applied a partial page transaction");
        }
        forwarder.join();
        require(publisher.pending_bytes() == 0 &&
            publisher.pending_messages() == 0, "publisher leaked group charge");
        require(one->closed() && (db_seven ? !old->closed() : old->closed()),
            "legacy/v1 page filtering or disconnection failed");
        pair.peer->close();
        legacy.peer->close();
        version_one.peer->close();
    });
    acl::fiber::schedule();
    controller.join();
    require(client->pending_bytes() == 0 && client->pending_messages() == 0 &&
        old->pending_messages() == 0 && one->pending_messages() == 0,
        "subscriber leaked group references/charges");
}

static void test_inflight_group() {
    std::vector<db::batch_entry> roots;
    roots.emplace_back("k", false, "root");
    roots.back().action = db::update_action::write;
    std::vector<page_change> pages;
    for (uint64_t id = 1; id <= 80; ++id) {
        pages.emplace_back("k", id, std::string(16384, 'x'),
            db::update_action::write);
    }
    auto batch = page_batch::capture(0, roots, pages);
    require(batch != nullptr, "inflight capture");
    streams pair;
    int capacity = 4096;
    require(::setsockopt(pair.server->sock_handle(), SOL_SOCKET, SO_SNDBUF,
        &capacity, sizeof(capacity)) == 0, "small send buffer");
    auto client = std::make_shared<slave_client>(pair.server,
        batch->size().bytes, batch->size().records);
    auto destination = memory();
    cluster_client receiver("", destination);
    go[client] { client->run(); };
    std::thread controller([&] {
        handshake(pair, 2);
        wait_drained(*client);
        require(receiver.apply(std::make_shared<kv_message>("", "",
            message_oper_ready, 0, 2)), "inflight READY");
        client->push(new kv_message(batch));
        pollfd descriptor{pair.peer->sock_handle(), POLLIN, 0};
        require(::poll(&descriptor, 1, 2000) == 1 &&
            client->pending_bytes() == batch->size().bytes &&
            client->pending_messages() == batch->size().records,
            "partially sent group lost its whole-transaction charge");
        client->push(new kv_message("extra", "value", message_oper_set));
        require(client->closed(), "inflight group exceeded output budget");
        rpc_packet packet;
        while (rpc_reader::read_packet(pair.peer, packet)) {
            require(packet.page && receiver.apply_packet(std::move(packet)),
                "inflight prefix packet");
        }
        std::string root;
        require(!destination->get("k", root),
            "disconnected partial group applied metadata");
        pair.peer->close();
    });
    acl::fiber::schedule();
    controller.join();
    require(client->pending_bytes() == 0 && client->pending_messages() == 0,
        "failed inflight group leaked charge/references");
}

static void test_shutdown_reader() {
    for (unsigned iteration = 0; iteration < 100; ++iteration) {
        streams pair;
        auto client = std::make_shared<slave_client>(pair.server, 1024, 8);
        if (iteration % 3 == 0) client->disconnect();
        go[client] { client->run(); };
        go[client, iteration] {
            if (iteration % 3 == 1) acl::fiber::yield();
            client->disconnect();
        };
        acl::fiber::schedule();
        require(client->closed() && client->pending_messages() == 0,
            "shutdown must join reader and drain queue");
    }
}

int main() {
    acl::acl_cpp_init();
    acl::fiber::acl_io_hook();
    for (unsigned limit = 0; limit <= 7; ++limit) {
        run_path(limit);
    }
    test_inflight_group();
    test_shutdown_reader();
    acl::fiber::acl_io_unlock();
    std::cout << "Live page publication/apply and queue limits passed\n";
}
