#pragma once
#include "move_memory_support.h"
#include "cluster/cluster_client.h"
#include "rpc/exec_stream.h"
#include "rpc/rpc_reader.h"
#include <future>
#include <sys/socket.h>

struct group_trace {
    unsigned prepared = 0, published = 0, failed_at = 0;
    bool no_allocate = false;
    std::function<void()> prepared_all;
};
struct traced_frame : db::prepared_batch {
    group_trace& trace;
    std::unique_ptr<db::prepared_batch> inner;
    explicit traced_frame(group_trace& value) : trace(value) {}
    void publish() noexcept override {
        inner->publish();
        if (++trace.published == 3 && trace.no_allocate) family_budget = -1;
    }
};
template<class Base> struct group_probe : family_probe<Base> {
    group_trace& trace;
    group_probe(group_trace& value, size_t shards)
    : family_probe<Base>(shards), trace(value) {}
protected:
    db::lookup_status dblookup(const std::string& key,
        std::string& out) override {
        if (this->fail_read) return db::lookup_status::error;
        return Base::dblookup(key, out);
    }
    db::batch_status dbprepare_owned_pages(
        const std::vector<db::batch_entry>& roots,
        const std::vector<page_change>& pages,
        std::unique_ptr<db::prepared_batch>& out) override {
        if (++trace.prepared == trace.failed_at)
            return db::batch_status::error;
        std::unique_ptr<traced_frame> frame(new traced_frame(trace));
        const auto status = Base::dbprepare_owned_pages(roots, pages,
            frame->inner);
        if (status != db::batch_status::ok) return status;
        out = std::move(frame);
        if (trace.prepared == 3) {
            if (trace.prepared_all) trace.prepared_all();
            if (trace.no_allocate) family_budget = 0;
        }
        return db::batch_status::ok;
    }
};
static shared_db group_database(bool tbb, group_trace& trace, size_t shards) {
#ifdef HAS_TBB
    if (tbb) return std::make_shared<group_probe<mdb_tbb>>(trace, shards);
#else
    require(!tbb, "group oneTBB unavailable");
#endif
    return std::make_shared<group_probe<mdb>>(trace, shards);
}
static std::shared_ptr<const page_batch> group_write(unsigned id,
    const std::string& key, size_t count, bool maintenance = false) {
    std::vector<db::batch_entry> roots;
    roots.emplace_back(key, true, encoded_root(count, family_deadline));
    roots.back().action = db::update_action::write;
    roots.back().clear_pages = true;
    roots.back().maintenance_only = maintenance;
    std::vector<page_change> pages;
    for (size_t page = 1; page <= count; ++page)
        pages.emplace_back(key, page, page_bytes(page),
            db::update_action::write);
    return page_batch::capture(id, roots, pages);
}
static std::shared_ptr<const page_batch> group_erase(unsigned id,
    const std::string& key) {
    std::vector<db::batch_entry> roots;
    roots.emplace_back(key, true, "");
    roots.back().action = db::update_action::erase;
    return page_batch::capture(id, roots, {});
}
struct group_fixture {
    group_trace trace;
    family_events events;
    db_watchers zero_events{0}, seven_events{7}, last_events{15};
    std::vector<shared_db> databases;
    std::unique_ptr<database_pool> pool;
    std::shared_ptr<const exec_batch> group;
    explicit group_fixture(bool tbb, bool mixed = false) {
        databases = {group_database(tbb, trace, 4),
            group_database(mixed ? !tbb : tbb, trace, 13),
            group_database(tbb, trace, 7)};
        db_watchers* watchers[] = {&zero_events, &seven_events, &last_events};
        for (size_t i = 0; i < 3; ++i) {
            watchers[i]->add_watcher(&events);
            require(databases[i]->open("", watchers[i]), "group memory open");
            seed(databases[i], family_key, i == 1 ? 5 : 3, family_deadline);
        }
        pool.reset(new database_pool(databases[0], 16, [this](size_t id) {
            return id == 7 ? databases[1] : id == 15 ? databases[2] :
                shared_db();
        }));
        group = exec_batch::capture({group_erase(0, family_key),
            group_write(7, family_key, 3), group_write(15, family_key, 2)});
        require(group != nullptr, "group capture"); events.calls = 0;
    }
    std::vector<memory_image> images() {
        std::vector<memory_image> out;
        for (auto database : databases)
            out.push_back(image(database, family_key));
        return out;
    }
    void complete() {
        const auto result = images();
        require(!result[0].found && result[0].pages.empty() &&
            result[1].root == encoded_root(3, family_deadline) &&
            result[1].pages.size() == 3 &&
            result[2].root == encoded_root(2, family_deadline) &&
            result[2].pages.size() == 2 && !events.calls,
            "group final image/observer echo");
        for (size_t i : {size_t(1), size_t(2)})
            for (const auto& page : result[i].pages) {
                const auto bytes = page_bytes(page.first);
                require(page.second.bytes == bytes.size() &&
                    page.second.crc == ::crc32(0,
                        reinterpret_cast<const Bytef*>(bytes.data()),
                        static_cast<uInt>(bytes.size())), "group page CRC");
            }
    }
};
struct group_streams {
    acl::shared_stream writer = std::make_shared<acl::socket_stream>();
    acl::shared_stream reader = std::make_shared<acl::socket_stream>();
    group_streams() {
        int sockets[2];
        require(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0,
            "group socketpair");
        require(writer->open(sockets[0]) && reader->open(sockets[1]),
            "group streams"); reader->set_rw_timeout(2);
    }
};
static std::vector<page_frame> group_frames(const exec_batch& input) {
    group_streams streams;
    auto writer = std::async(std::launch::async, [&] {
        return exec_stream::send(streams.writer, input);
    });
    std::vector<page_frame> out;
    for (size_t i = 0; i < input.records(); ++i) {
        rpc_packet packet;
        require(rpc_reader::read_packet(streams.reader, packet) && packet.page,
            "group read"); out.push_back(std::move(packet.frame));
    }
    require(writer.get(), "group send"); return out;
}
