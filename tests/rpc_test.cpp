#include "stdafx.h"
#include "native_string_fixture.h"
#include "rpc/rpc_sender.h"
#include "rpc/rpc_reader.h"
#include "rpc/rpc_message.h"
#include "rpc/page_wire.h"
#include "slave/slave_client.h"
#include "slave/slave_watcher.h"
#include "db/db_watcher.h"
#include "common/replica_budget.h"
#include "cluster/cluster_client.h"
#include "db/database_pool.h"
#include <sys/socket.h>
#include <unistd.h>
#include <poll.h>
#include <thread>
#include <iostream>

static void require(bool ok, const char* message) {
    if (!ok) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

struct streams {
    acl::shared_stream writer = std::make_shared<acl::socket_stream>();
    acl::shared_stream reader = std::make_shared<acl::socket_stream>();
    streams() {
        int sockets[2];
        require(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0,
            "socketpair failed");
        require(writer->open(sockets[0]) && reader->open(sockets[1]),
            "open streams failed");
        writer->set_rw_timeout(2);
        reader->set_rw_timeout(2);
    }
};

static void test_legacy_frames(const std::string& binary) {
    using namespace pkv;
    for (const auto& key : {std::string(), binary}) {
        for (const auto& value : {std::string(), binary}) {
            for (auto operation : {message_oper_set, message_oper_get,
                message_oper_del}) {
                if (operation == message_oper_del && !value.empty()) {
                    continue;
                }
                streams pair;
                kv_message message(key, value, operation);
                require(rpc_sender::send(pair.writer, &message),
                    "single send failed");
                shared_message received;
                require(rpc_reader::read(pair.reader, received) &&
                    received->get_key() == key &&
                        received->get_value() == value && received->get_oper()
                            == operation,
                        "single RPC frame changed empty/binary payload");
            }
        }
    }
    {
        streams pair;
        kv_message first("", binary, message_oper_set);
        kv_message second(binary, "", message_oper_del);
        std::vector<kv_message*> batch{&first, &second};
        require(!rpc_sender::send(pair.writer, batch, 1),
            "undersized batch capacity accepted");
        pollfd descriptor{pair.reader->sock_handle(), POLLIN, 0};
        require(::poll(&descriptor, 1, 0) == 0,
            "failed batch emitted a prefix");
        require(rpc_sender::send(pair.writer, batch), "batch send failed");
        for (const auto message : batch) {
            shared_message received;
            require(rpc_reader::read(pair.reader, received) &&
                received->get_key() == message->get_key() &&
                    received->get_value() == message->get_value() &&
                        received->get_oper() == message->get_oper(),
                    "batch lost frame boundary after empty key");
        }
    }
    {
        streams pair;
        kv_message maximum(std::string(MESSAGE_KEY_MAX, 'k'),
            std::string(MESSAGE_VAL_MAX, 'v'), message_oper_set);
        bool sent = false;
        std::thread sender([&] { sent = rpc_sender::send(pair.writer, &maximum);
            });
        shared_message received;
        require(rpc_reader::read(pair.reader, received) && received->get_key()
            == maximum.get_key() &&
                received->get_value() == maximum.get_value(),
                    "maximum legacy frame failed");
        sender.join();
        require(sent, "maximum send failed");
    }
}

static void test_rejected_frames() {
    using namespace pkv;
    for (const auto header : {rpc_message_hdr{0, 0, 0, 0, 0}, rpc_message_hdr{4,
        0, 0, 0, 0},
            rpc_message_hdr{1, MESSAGE_KEY_MAX + 1, 0, 0, 0}, rpc_message_hdr{1,
                0, MESSAGE_VAL_MAX + 1, 0, 0},
            rpc_message_hdr{3, 0, 1, 0, 0}, rpc_message_hdr{1, 0, 0, 1, 0},
                rpc_message_hdr{1, 0, 0, 0, 1}}) {
        streams pair;
        require(pair.writer->write(&header, sizeof(header)) == sizeof(header),
            "invalid header write failed");
        shared_message stale = std::make_shared<kv_message>("stale", "old",
            message_oper_set);
        require(!rpc_reader::read(pair.reader, stale) && !stale,
            "invalid RPC header accepted or retained old output");
    }
    for (bool header_only : {false, true}) {
        streams pair;
        rpc_message_hdr header{1, 3, 4, 0, 0};
        if (header_only) {
            require(pair.writer->write(&header, 7) == 7,
                "partial header write failed");
        } else {
            require(pair.writer->write(&header, sizeof(header)) ==
                sizeof(header) &&
                    pair.writer->write("keyxy", 5) == 5,
                        "truncated payload write failed");
        }
        pair.writer->close();
        shared_message stale = std::make_shared<kv_message>("stale", "old",
            message_oper_set);
        require(!rpc_reader::read(pair.reader, stale) && !stale,
            "truncated RPC frame accepted");
    }
    {
        streams pair;
        rpc_message_hdr header{1, 0, 3, 0, 0};
        std::string bytes(reinterpret_cast<char*>(&header), sizeof(header));
        bytes += "abc";
        std::thread sender([&] {
            for (char byte : bytes) {
                require(pair.writer->write(&byte, 1) == 1,
                    "fragment write failed");
            }
        });
        shared_message received;
        require(rpc_reader::read(pair.reader, received) &&
            received->get_key().empty() && received->get_value() == "abc",
                "fragmented empty-key RPC frame failed");
        sender.join();
    }
    {
        streams pair;
        kv_message valid("key", "value", message_oper_set);
        kv_message invalid("key", std::string(MESSAGE_VAL_MAX + 1, 'x'),
            message_oper_set);
        kv_message invalid_delete("key", "value", message_oper_del);
        require(!rpc_sender::send(pair.writer, &invalid) &&
            !rpc_sender::send(pair.writer, &invalid_delete) &&
                !rpc_sender::send(pair.writer, std::vector<kv_message*>{&valid,
                    &invalid}), "invalid outbound frame accepted");
        pollfd descriptor{pair.reader->sock_handle(), POLLIN, 0};
        require(::poll(&descriptor, 1, 0) == 0,
            "invalid outbound batch partially sent");
    }
    {
        streams pair;
        auto client = std::make_shared<slave_client>(pair.writer);
        client->push(new kv_message("key", "", message_oper_max));
        for (int i = 0; i < 20; ++i) {
            client->push(new kv_message("queued", "value", message_oper_set));
        }
        go[client] { client->run(); };
        acl::fiber::schedule();
        // Rejected pushes must release their owned reference after EOF too.
        client->push(new kv_message("after-close", "value", message_oper_set));
    }
}

static void test_versioned_frames(const std::string& binary) {
    using namespace pkv;
    for (unsigned version : {1U, 2U, 3U, 4U, 5U}) {
        streams pair;
        kv_message ready("", "", message_oper_ready);
        kv_message zero("same", native_string_fixture("zero"), message_oper_set);
        kv_message one("same", native_string_fixture("one"), message_oper_set, 1);
        kv_message binary_message("", native_string_fixture(binary), message_oper_set, 15);
        kv_message deleted("same", "", message_oper_del, 1);
        std::vector<kv_message*> frames{&ready, &zero, &one, &binary_message,
            &deleted};
        require(!rpc_sender::send(pair.writer, &one),
            "legacy sender accepted a nonzero DB");
        require(rpc_sender::send_versioned(pair.writer, frames, version),
            "versioned send failed");
        auto primary = db::create_mdb(2);
        require(primary->open(""), "replica DB 0 open failed");
        auto databases = std::make_shared<database_pool>(primary, 16, [](size_t)
            {
            auto created = db::create_mdb(2);
            return created->open("") ? created : nullptr;
        });
        cluster_client receiver("", primary, databases);
        require(!receiver.apply(std::make_shared<kv_message>("same", "early",
            message_oper_set, 1)),
                "receiver applied DB N before READY");
        for (const auto expected : frames) {
            shared_message received;
            require(rpc_reader::read(pair.reader, received) &&
                received->get_database() == expected->get_database() &&
                    received->get_oper() == expected->get_oper() &&
                        received->get_key() == expected->get_key() &&
                    received->get_value() == expected->get_value() &&
                        receiver.apply(received),
                        "versioned read/apply failed");
        }
        std::string value;
        auto one_db = databases->get(1);
        auto last_db = databases->get(15);
        require(primary->get("same", value) && value == zero.get_value() &&
            !one_db->get("same", value) && last_db->get("", value) && value ==
            binary_message.get_value(),
                "versioned apply crossed database boundaries");
        require(receiver.apply(std::make_shared<kv_message>("same", "",
            message_oper_del, 1, version)), "replayed delete failed");
        require(!receiver.apply(std::make_shared<kv_message>("same", "invalid",
            message_oper_set, 16, version)),
            "out-of-range replica DB accepted");
        require(!receiver.apply(std::make_shared<kv_message>("", "",
            message_oper_ready, 0, version)), "duplicate READY accepted");
    }
    {
        streams pair;
        kv_message message("abc", "def", message_oper_set, 7);
        require(rpc_sender::send_versioned(pair.writer, {&message}),
            "golden frame send failed");
        const unsigned char expected[] = {0x50, 0x4b, 0x56, 0x31, 0, 0, 0, 1, 0,
            0, 0, 7, 0, 0, 0, 3, 0, 0, 0, 3};
        unsigned char header[20];
        char body[6];
        require(pair.reader->read(header, sizeof(header), true) ==
            sizeof(header) &&
                std::memcmp(header, expected, sizeof(header)) == 0 &&
                    pair.reader->read(body, sizeof(body), true) == sizeof(body)
                    &&
                std::string(body, sizeof(body)) == "abcdef",
                    "versioned wire bytes are not canonical network order");
    }
    for (const auto header : {rpc_message_hdr{htonl(rpc_version_one), htonl(1),
        htonl(65536), 0, 0},
            rpc_message_hdr{htonl(rpc_version_one), htonl(4), htonl(1), 0, 0},
            rpc_message_hdr{htonl(rpc_version_one), htonl(4), 0, htonl(1), 0},
            rpc_message_hdr{htonl(rpc_version_one), htonl(5), 0, 0, 0},
            rpc_message_hdr{htonl(page_wire::compact_magic + 1), htonl(1),
                0, 0, 0},
            rpc_message_hdr{htonl(rpc_version_one), htonl(1), 0,
                htonl(MESSAGE_KEY_MAX + 1), 0},
            rpc_message_hdr{htonl(rpc_version_one), htonl(1), 0, 0,
                htonl(MESSAGE_VAL_MAX + 1)}}) {
        streams pair;
        require(pair.writer->write(&header, sizeof(header)) == sizeof(header),
            "invalid versioned header write failed");
        shared_message output;
        require(!rpc_reader::read(pair.reader, output) && !output,
            "invalid versioned header accepted");
    }
}

static void test_budget_arithmetic() {
    using namespace pkv;
    {
        replica_budget budget(209, 7);
        require(!budget.reserve_encoded(SIZE_MAX, 1) &&
            !budget.reserve_encoded(SIZE_MAX, SIZE_MAX) &&
            !budget.reserve_encoded(0, 0) && budget.reserve_encoded(209, 7),
            "encoded group arithmetic/boundary failure");
        require(!budget.reserve_encoded(20, 1), "group exceeded message cap");
        budget.release_encoded(209, 7);
        require(budget.bytes() == 0 && budget.messages() == 0,
            "encoded group release mismatch");
    }
    {
        replica_budget budget(44, 2);
        require(budget.reserve(1, 1) && budget.reserve(1, 1) && budget.bytes()
            == 44 && budget.messages() == 2,
                "replica budget rejected exact capacity");
        require(!budget.reserve(0, 0) && budget.bytes() == 44 &&
            budget.messages() == 2,
                "replica budget exceeded byte/record cap");
        budget.release(1, 1);
        require(!budget.reserve(static_cast<size_t>(-1), 0) &&
            !budget.reserve(0, static_cast<size_t>(-1)),
                "replica budget wrapped a hostile length");
        budget.release(1, 1);
        require(budget.bytes() == 0 && budget.messages() == 0,
            "replica budget did not recover capacity");
    }
}

static void test_subscriber_budget() {
    using namespace pkv;
    for (bool byte_limit : {false, true}) {
        streams pair;
        auto client = std::make_shared<slave_client>(pair.writer, byte_limit ?
            44 : 1024, byte_limit ? 10 : 2);
        client->push(new kv_message("k", "v", message_oper_set));
        client->push(new kv_message("k", "v", message_oper_set));
        require(client->pending_bytes() == 44 && client->pending_messages() == 2
            && !client->closed(),
                "subscriber did not charge pending records");
        client->push(new kv_message("k", "v", message_oper_set));
        require(client->closed() && client->pending_bytes() == 44 &&
            client->pending_messages() == 2,
                "subscriber overflow did not close at its boundary");
        client->push(new kv_message("rejected", "value", message_oper_set));
        go[client] { client->run(); };
        acl::fiber::schedule();
        require(client->pending_bytes() == 0 && client->pending_messages() == 0,
            "closed subscriber leaked budget charges");
    }
    {
        streams pair;
        auto client = std::make_shared<slave_client>(pair.writer, 19, 1);
        require(pair.reader->write(rpc_upgrade_request,
            std::strlen(rpc_upgrade_request)) ==
            std::strlen(rpc_upgrade_request),
                "upgrade budget test request failed");
        go[client] { client->run(); };
        acl::fiber::schedule();
        require(client->closed() && client->pending_bytes() == 0,
            "READY bypassed output budget");
    }
    {
        streams pair;
        int capacity = 4096;
        require(::setsockopt(pair.writer->sock_handle(), SOL_SOCKET, SO_SNDBUF,
            &capacity, sizeof(capacity)) == 0,
                "set small socket send buffer failed");
        const size_t size = MESSAGE_VAL_MAX + 21;
        auto client = std::make_shared<slave_client>(pair.writer, size, 10);
        client->push(new kv_message("k", std::string(MESSAGE_VAL_MAX, 'v'),
            message_oper_set));
        go[client] { client->run(); };
        std::thread interrupt([client, &pair, size] {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            pollfd descriptor{pair.reader->sock_handle(), POLLIN, 0};
            require(::poll(&descriptor, 1, 0) == 1
                    && client->pending_bytes() == size
                    && client->pending_messages() == 1,
                    "in-flight write was released before send completion");
            client->push(new kv_message("next", "value", message_oper_set));
            require(client->closed(),
                    "in-flight cap did not disconnect subscriber");
        });
        acl::fiber::schedule();
        interrupt.join();
        require(client->pending_bytes() == 0 && client->pending_messages() == 0,
                "shutdown did not wake blocked writer and drain its charges");
    }
}

static void test_publisher_budget() {
    using namespace pkv;
    for (bool byte_limit : {false, true}) {
        streams old_pair, fresh_pair;
        auto old_client = std::make_shared<slave_client>(old_pair.writer);
        auto fresh_client = std::make_shared<slave_client>(fresh_pair.writer);
        slave_watcher publisher(byte_limit ? 44 : 1024, byte_limit ? 10 : 2);
        publisher.add_client(old_client);
        db_watchers callbacks;
        callbacks.add_watcher(&publisher);
        require(callbacks.on_set("k", "v", false) && callbacks.on_get("k", "v",
            true) && publisher.pending_messages() == 0,
                "failed mutation/read consumed publication budget");
        callbacks.on_set("k", "v", true);
        callbacks.on_set("k", "v", true);
        require(publisher.pending_bytes() == 44 && publisher.pending_messages()
            == 2, "publisher did not charge queue capacity");
        callbacks.on_set("k", "v", true);
        require(old_client->closed(),
            "publication overflow did not invalidate subscribers");
        publisher.add_client(fresh_client);
        publisher.stop();
        std::thread consumer([&] { publisher.run(); });
        consumer.join();
        require(publisher.pending_bytes() == 0 && publisher.pending_messages()
            == 0 && fresh_client->pending_messages() == 0,
                "stale publication crossed overflow generation or leaked "
                    "charges");
    }
}

static void test_closed_streams() {
    using namespace pkv;
    acl::shared_stream missing;
    auto closed = std::make_shared<acl::socket_stream>();
    for (auto stream : {missing, closed}) {
        rpc_packet packet;
        packet.page = true;
        packet.message = std::make_shared<kv_message>("old", "bytes",
            message_oper_set);
        require(!rpc_reader::read_packet(stream, packet) && !packet.page &&
            !packet.message, "closed stream retained packet or entered read");
        shared_message message = std::make_shared<kv_message>("old", "v",
            message_oper_set);
        require(!rpc_reader::read(stream, message) && !message,
            "closed stream retained message");
        std::string bytes("old");
        require(!rpc_reader::read(stream, 1, bytes) && bytes == "old",
            "closed stream entered payload read");
        require(rpc_reader::read(stream, 0, bytes) && bytes.empty(),
            "zero-byte read required a connection");
    }
}

int main() {
    test_closed_streams();
    static_assert(sizeof(pkv::rpc_message_hdr) == 20,
        "legacy RPC header layout changed");
    std::string binary;
    for (int i = 0; i < 256; ++i) {
        binary.push_back(static_cast<char>(i));
    }
    test_legacy_frames(binary);
    test_rejected_frames();
    test_versioned_frames(binary);
    test_budget_arithmetic();
    test_subscriber_budget();
    test_publisher_budget();
    std::cout << "RPC transport and replica queue tests passed\n";
}
