#pragma once
#include "replica_group_support.h"
#include "action/redis_handler.h"
#include "action/redis_service.h"
#include "coder/redis_ocache.h"
#include "dao/paged_string.h"
#include <thread>

struct retention_capture : family_events {
    std::vector<std::shared_ptr<const page_batch>> batches;
    bool on_pages_database(unsigned id,
        const std::vector<db::batch_entry>& roots,
        const std::vector<page_change>& pages) override {
        auto batch = page_batch::capture(id, roots, pages);
        require(batch != nullptr, "retention capture");
        batches.push_back(batch); return true;
    }
};
struct retention_connection {
    redis_ocache cache{64};
    redis_coder parser{cache};
    redis_service service;
    acl::socket_stream writer, reader;
    std::unique_ptr<redis_handler> handler;
    retention_connection(shared_db zero, bool replica, shared_db one = {}) {
        int sockets[2];
        require(!::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets), "socketpair");
        require(writer.open(sockets[0]) && reader.open(sockets[1]), "streams");
        service.replica(replica);
        auto pool = std::make_shared<database_pool>(zero, 2, [one](size_t) {
            return one;
        });
        handler.reset(new redis_handler(service, pool, parser, writer));
    }
    std::string run(const std::vector<std::string>& args) {
        parser.clear();
        std::string wire = "*" + std::to_string(args.size()) + "\r\n";
        for (const auto& arg : args)
            wire += "$" + std::to_string(arg.size()) + "\r\n" + arg + "\r\n";
        size_t size = wire.size(); parser.update(wire.data(), size);
        require(!size && handler->handle(), "retention handler");
        std::string out; char bytes[4096];
        for (;;) {
            const auto count = ::recv(reader.sock_handle(), bytes,
                sizeof(bytes), MSG_DONTWAIT);
            if (count <= 0) break;
            out.append(bytes, static_cast<size_t>(count));
        }
        return out;
    }
};
struct retention_pair {
    retention_capture changes;
    family_events effects;
    db_watchers primary_events{0}, replica_events{0};
    shared_db primary, replica;
    std::unique_ptr<retention_connection> source, client;
    explicit retention_pair(bool tbb) {
        primary = memory_database(tbb, 4); replica = memory_database(tbb, 13);
        primary_events.add_watcher(&changes);
        replica_events.add_watcher(&effects);
        require(primary->open("", &primary_events) &&
            replica->open("", &replica_events), "retention memory open");
        source.reset(new retention_connection(primary, false));
        client.reset(new retention_connection(replica, true));
    }
};
static const std::string retention_key("retain\0key", 10);
static const std::string retention_large(49152, 'x');
static std::vector<std::string> retention_seed(unsigned type) {
    switch (type) {
    case 0: return {"SET", retention_key, std::string(70000, 'x')};
    case 1: return {"HSET", retention_key, "large", retention_large};
    case 2: return {"RPUSH", retention_key, retention_large};
    case 3: return {"SADD", retention_key, retention_large};
    default: return {"ZADD", retention_key, "1", retention_large};
    }
}
static std::vector<std::string> retention_delta(unsigned type) {
    switch (type) {
    case 0: return {"SETRANGE", retention_key, "16384", "y"};
    case 1: return {"HSET", retention_key, "new", "y"};
    case 2: return {"LPUSH", retention_key, "new"};
    case 3: return {"SADD", retention_key, "new"};
    default: return {"ZADD", retention_key, "2", "new"};
    }
}
static void retention_value(shared_db& database, unsigned type) {
    std::string value;
    if (!type) {
        dao::paged_string data; auto expected = std::string(70000, 'x');
        expected[16384] = 'y';
        require(data.get(database, retention_key, value) && value == expected,
            "delayed String lost unchanged pages");
    } else if (type == 1) {
        dao::paged_hash data;
        require(data.hget(database, retention_key, "large", value) &&
            value == retention_large, "delayed Hash lost blob");
        require(data.hget(database, retention_key, "new", value) &&
            value == "y",
            "delayed Hash delta lost");
    } else if (type == 2) {
        dao::paged_list data; std::vector<std::string> values;
        require(data.range(database, retention_key, 0, -1, values) &&
            values == std::vector<std::string>({"new", retention_large}),
            "delayed List lost unchanged blob");
    } else if (type == 3) {
        dao::paged_set data; std::set<std::string> values;
        require(data.members(database, retention_key, values) &&
            values == std::set<std::string>({"new", retention_large}),
            "delayed Set lost unchanged blob");
    } else {
        dao::paged_zset data; std::vector<zset_member> values;
        require(data.range(database, retention_key, 0, -1, false, values) &&
            values.size() == 2 && values[0].member == retention_large &&
            values[0].score == 1 && values[1].member == "new" &&
            values[1].score == 2, "delayed ZSet lost unchanged member");
    }
}
