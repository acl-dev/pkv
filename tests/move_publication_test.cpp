#include "stdafx.h"
#include "native_string_fixture.h"
#include "move_memory_support.h"
#include "slave/slave_watcher.h"
#include "slave/replica_scope.h"
#include "rpc/move_intent.h"
#include "rpc/rpc_reader.h"
#include "cluster/cluster_client.h"
#include <chrono>
#include <thread>
#include <sys/socket.h>
#include <unistd.h>

struct publication_streams {
    acl::shared_stream writer = std::make_shared<acl::socket_stream>();
    acl::shared_stream reader = std::make_shared<acl::socket_stream>();
    publication_streams() {
        int sockets[2];
        require(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0,
            "publication socketpair");
        require(writer->open(sockets[0]) && reader->open(sockets[1]),
            "publication socket open");
        reader->set_rw_timeout(5);
    }
};
struct move_source {
    slave_watcher publisher;
    db_watchers from_events{0}, to_events{7};
    shared_db from, to;
    database_pool pool;
    bool drained = false;
    explicit move_source(bool tbb = false, size_t bytes = 64 * 1024 * 1024,
        size_t records = 65536)
    : publisher(bytes, records), from(memory_database(tbb, 4)),
      to(memory_database(tbb, 9)),
      pool(from, 16, [this](size_t id) {
          return id == 7 ? to : shared_db();
      }) {
        from_events.add_watcher(&publisher);
        to_events.add_watcher(&publisher);
        require(from->open("", &from_events) && to->open("", &to_events),
            "observed pair open");
        seed_memory(from, family_key, 3, family_deadline);
    }
    void drain() {
        if (!drained) { publisher.stop(); publisher.run(); drained = true; }
    }
    ~move_source() { drain(); }
    family_move intent(bool reverse = false) {
        family_move out;
        out.source = reverse ? 7 : 0; out.target = reverse ? 0 : 7;
        out.key = family_key;
        out.source_root = image(reverse ? to : from, family_key).root;
        return out;
    }
    db::batch_status transfer(bool reverse, bool skip, long allowance,
        bool& moved) {
        execution_guard admission(true);
        auto source = reverse ? to : from, target = reverse ? from : to;
        std::unique_ptr<family_publication> publication;
        move_memory::callbacks actions;
        actions.prepare = [&](const move_memory::roots& roots) {
            family_move move;
            move.source = reverse ? 7 : 0; move.target = reverse ? 0 : 7;
            move.key = roots.owner; move.source_root = roots.source;
            move.target_found = roots.target_found;
            move.target_root = roots.target;
            family_budget = allowance;
            const bool ok = source->prepare_move_observers(*target, move,
                publication);
            family_budget = -1;
            move.source_root = "caller changed its snapshot";
            return !ok ? move_memory::choice::error : skip ?
                move_memory::choice::skip : move_memory::choice::proceed;
        };
        actions.complete = [&] { if (publication) publication->publish(); };
        actions.observe = [&](move_memory::step at) {
            if (at == move_memory::step::published) family_budget = 0;
        };
        const auto status = move_memory::run(pool, reverse ? 7 : 0,
            reverse ? 0 : 7, family_key, actions, moved);
        family_budget = -1; publication.reset(); return status;
    }
};
static shared_client attach(move_source& source, publication_streams& pair) {
    auto client = std::make_shared<slave_client>(pair.writer);
    source.publisher.add_client(client); return client;
}
static void cancellation(bool grouped) {
    move_source source;
    publication_streams pair;
    auto client = attach(source, pair);
    execution_guard admission(true);
    std::unique_ptr<replica_scope> scope;
    if (grouped) {
        scope.reset(new replica_scope(&source.publisher));
        require(source.from->set("prefix", "retained"), "prefix set");
    }
    const auto bytes = source.publisher.pending_bytes();
    const auto records = source.publisher.pending_messages();
    std::unique_ptr<family_publication> prepared;
    require(source.from->prepare_move_observers(*source.to, source.intent(),
        prepared) && prepared && source.publisher.pending_bytes() > bytes,
        "MOVE reservation absent");
    prepared.reset();
    require(source.publisher.pending_bytes() == bytes &&
        source.publisher.pending_messages() == records && !client->closed(),
        "cancelled MOVE retained budget or invalidated prefix");
    bool moved = true;
    const auto before = image(source.from, family_key);
    require(source.transfer(false, true, -1, moved) == db::batch_status::ok &&
        !moved && image(source.from, family_key) == before &&
        source.publisher.pending_bytes() == bytes,
        "skipped MOVE published or retained a reservation");
    scope.reset();
}
static void prepare_faults(bool grouped) {
    unsigned failures = 0, successes = 0;
    for (long allowance = 0; allowance < 40; ++allowance) {
        move_source source;
        publication_streams pair;
        auto client = attach(source, pair);
        execution_guard admission(true);
        std::unique_ptr<replica_scope> scope;
        if (grouped) scope.reset(new replica_scope(&source.publisher));
        const auto before = image(source.from, family_key);
        bool moved = false;
        const auto status = source.transfer(false, false, allowance, moved);
        if (status == db::batch_status::ok) {
            ++successes;
            require(moved && image(source.to, family_key) == before,
                "successful observed MOVE lost native pages");
        } else {
            ++failures;
            require(!moved && image(source.from, family_key) == before &&
                !image(source.to, family_key).found && !client->closed() &&
                !source.publisher.pending_bytes() &&
                !source.publisher.pending_messages(),
                "failed MOVE changed state, peers or queue budget");
        }
        scope.reset();
    }
    require(failures && successes, "MOVE capture fault sweep lacks paths");
}
static void queue_limits(bool grouped) {
    move_source source(false, 1, 1);
    publication_streams pair;
    auto client = attach(source, pair);
    execution_guard admission(true);
    std::unique_ptr<replica_scope> scope;
    if (grouped) scope.reset(new replica_scope(&source.publisher));
    const auto before = image(source.from, family_key);
    bool moved = false;
    require(source.transfer(false, false, -1, moved) == db::batch_status::error
        && !moved && image(source.from, family_key) == before &&
        !source.publisher.pending_bytes() && !client->closed(),
        "queue limit was discovered after native publication");
    scope.reset();
}
static void native_prepare_failure(bool grouped) {
    move_source source;
    publication_streams pair;
    auto client = attach(source, pair);
    execution_guard admission(true);
    std::unique_ptr<replica_scope> scope;
    if (grouped) scope.reset(new replica_scope(&source.publisher));
    const auto before = image(source.from, family_key);
    control(source.to).fail_prepare = true;
    bool moved = false;
    require(source.transfer(false, false, -1, moved) == db::batch_status::error
        && !moved && image(source.from, family_key) == before &&
        !image(source.to, family_key).found && !client->closed() &&
        !source.publisher.pending_bytes() &&
        !source.publisher.pending_messages(),
        "native failure retained prepared ownership or changed family");
    scope.reset();
}
static void stopped_publication(bool grouped) {
    move_source source;
    publication_streams pair;
    auto client = attach(source, pair);
    execution_guard admission(true);
    std::unique_ptr<replica_scope> scope;
    if (grouped) scope.reset(new replica_scope(&source.publisher));
    std::unique_ptr<family_publication> prepared;
    require(source.from->prepare_move_observers(*source.to, source.intent(),
        prepared) && prepared, "stop preparation");
    source.publisher.stop(); prepared->publish(); prepared.reset();
    scope.reset();
    require(client->closed() && !source.publisher.pending_bytes() &&
        !source.publisher.pending_messages(), "stop retained MOVE budget");
}
struct counted_observer : family_events {
    unsigned active = 0, committed = 0, cancelled = 0, prepared = 0;
    bool reject = false;
    struct token : family_publication {
        counted_observer& owner;
        bool pending = true;
        explicit token(counted_observer& value) : owner(value) {
            ++owner.active; ++owner.prepared;
        }
        ~token() override {
            if (pending) { --owner.active; ++owner.cancelled; }
        }
        void publish() noexcept override {
            if (!pending) return;
            pending = false; --owner.active; ++owner.committed;
        }
    };
    bool prepare_move(const family_move&,
        std::unique_ptr<family_publication>& out) override {
        out.reset();
        if (reject) return false;
        out.reset(new token(*this)); return true;
    }
};
static void paired_source_rejection(bool grouped) {
    move_source source;
    counted_observer rejecting;
    rejecting.reject = true; source.to_events.add_watcher(&rejecting);
    publication_streams pair;
    auto client = attach(source, pair);
    execution_guard admission(true);
    std::unique_ptr<replica_scope> scope;
    if (grouped) {
        scope.reset(new replica_scope(&source.publisher));
        require(source.from->set("prefix", "retained"), "rejection prefix");
    }
    const auto bytes = source.publisher.pending_bytes();
    const auto records = source.publisher.pending_messages();
    const auto before = image(source.from, family_key);
    bool moved = false;
    require(source.transfer(false, false, -1, moved) == db::batch_status::error
        && !moved && image(source.from, family_key) == before &&
        !image(source.to, family_key).found && !client->closed() &&
        source.publisher.pending_bytes() == bytes &&
        source.publisher.pending_messages() == records,
        "later observer rejection retained replica reservation");
    scope.reset();
}
static void paired_observers() {
    counted_observer shared, later;
    db_watchers from(0), to(7);
    from.add_watcher(&shared); to.add_watcher(&shared);
    to.add_watcher(&later);
    family_move move;
    move.source = 0; move.target = 7; move.key = family_key;
    execution_guard admission(true);
    std::unique_ptr<family_publication> prepared;
    later.reject = true;
    require(!db_watchers::prepare_move(&from, &to, move, prepared) &&
        !prepared && shared.prepared == 1 && shared.cancelled == 1 &&
        !shared.active && !shared.committed, "paired observer rollback");
    later.reject = false;
    require(db_watchers::prepare_move(&from, &to, move, prepared) && prepared,
        "paired observer prepare");
    prepared->publish(); prepared->publish(); prepared.reset();
    require(shared.committed == 1 && later.committed == 1 &&
        shared.prepared == 2 && !shared.active && !later.active,
        "shared observer published twice");
    family_events unsupported;
    to.add_watcher(&unsupported);
    require(!db_watchers::prepare_move(&from, &to, move, prepared) &&
        !prepared && !shared.active && !later.active,
        "unsupported observer received a root-only fallback");
}
static void normalized_limit(bool bytes) {
    move_source source;
    publication_streams pair;
    auto client = attach(source, pair);
    execution_guard admission(true);
    replica_scope scope(&source.publisher);
    auto move = source.intent();
    if (bytes) move.key.assign(page_batch::max_key_bytes, 'k');
    size_t wire;
    require(move_intent::measure(move, wire), "limit measurement");
    const auto charge = 64 + 2 * move.key.size() + move.source_root.size();
    const auto limit = bytes ? page_batch::max_bytes / charge :
        page_batch::max_records / 3;
    size_t count = 0;
    for (; count < limit + 1; ++count) {
        std::unique_ptr<family_publication> prepared;
        if (!source.from->prepare_move_observers(*source.to, move, prepared))
            break;
        require(prepared != nullptr, "limit lost capture");
        prepared->publish();
    }
    require(count == limit && source.publisher.pending_bytes() ==
        56 + count * wire && source.publisher.pending_messages() == count + 2
        && source.publisher.pending_bytes() < page_batch::max_bytes &&
        !client->closed(), "normalized limit applied after publication");
}
static rpc_packet negotiate(publication_streams& pair, unsigned version) {
    const auto request = "PKV-REPL " + std::to_string(version) + "\r\n";
    require(pair.reader->write(request.data(), request.size()) ==
        static_cast<int>(request.size()), "publication greeting");
    rpc_packet packet;
    require(rpc_reader::read_packet(pair.reader, packet) && !packet.page &&
        packet.message->get_protocol() == version &&
        packet.message->get_oper() == message_oper_ready, "publication READY");
    return packet;
}
static void settled(slave_client& client) {
    for (unsigned i = 0; i < 1000 && client.pending_messages(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    require(!client.pending_messages(), "publication READY not drained");
}
static void edit_page(shared_db database, uint64_t id, uint64_t content) {
    require(database->transact_pages({family_key}, [&](page_transaction& tx) {
        return tx.put(family_key, id, page_bytes(content));
    }) == db::batch_status::ok, "observed page edit");
}
static void logical_image(const memory_image& expected,
    const memory_image& actual) {
    require(expected.found == actual.found && expected.root == actual.root &&
        expected.pages.size() == actual.pages.size(), "logical family roots");
    for (const auto& page : expected.pages) {
        const auto found = actual.pages.find(page.first);
        require(found != actual.pages.end() &&
            found->second.bytes == page.second.bytes &&
            found->second.crc == page.second.crc, "logical family pages");
    }
}
static void live_source(bool tbb, bool cancel) {
    move_source source(tbb);
    publication_streams pair, older;
    auto client = attach(source, pair), old = attach(source, older);
    auto zero = memory_database(tbb, 3), seven = memory_database(tbb, 5);
    require(zero->open("") && seven->open(""), "receiver pair open");
    seed_memory(zero, family_key, 3, family_deadline);
    auto pool = std::make_shared<database_pool>(zero, 16, [&](size_t id) {
        return id == 7 ? seven : shared_db();
    });
    cluster_client receiver("", zero, pool);
    const auto before = image(zero, family_key);
    const auto native = image(source.from, family_key);
    go[client] { client->run(); };
    go[old] { old->run(); };
    std::thread controller([&] {
        require(receiver.apply_packet(negotiate(pair, 4)), "receiver ready");
        negotiate(older, 3); settled(*client); settled(*old);
        {
            replica_scope scope(&source.publisher);
            require(source.from->set("prefix", native_string_fixture("one")), "source prefix");
            if (cancel) {
                std::unique_ptr<family_publication> prepared;
                require(source.from->prepare_move_observers(*source.to,
                    source.intent(), prepared) && prepared,
                    "cancelled source prepare");
                prepared.reset();
            } else {
                bool moved = false;
                require(source.transfer(false, false, -1, moved) ==
                    db::batch_status::ok && moved, "source forward MOVE");
                edit_page(source.to, 1, 91);
                require(source.transfer(true, false, -1, moved) ==
                    db::batch_status::ok && moved, "source reverse MOVE");
                edit_page(source.from, 3, 93);
            }
            require(source.from->set("suffix", native_string_fixture("two")) &&
                !client->pending_messages() && !old->pending_messages(),
                "source forwarded an uncommitted prefix");
        }
        const auto records = source.publisher.pending_messages();
        require(records > 2, "source EXEC did not capture observations");
        std::thread forwarder([&] { source.drain(); });
        unsigned moves = 0;
        std::vector<unsigned> databases;
        for (size_t i = 0; i < records; ++i) {
            rpc_packet packet;
            require(rpc_reader::read_packet(pair.reader, packet) &&
                packet.page, "source ordered packet");
            const auto operation = packet.frame.operation;
            if (operation == page_operation::exec_begin)
                require(page_wire::get32(packet.frame.value.data()) ==
                    (cancel ? 2 : 6), "source entry order/count");
            if (operation == page_operation::begin)
                databases.push_back(packet.frame.database);
            if (operation == page_operation::family_move) ++moves;
            require(receiver.apply_packet(std::move(packet)),
                "source ordered replay rejected immutable roots");
            if (i + 1 != records) {
                require(image(zero, family_key) == before,
                    "source group published a native prefix");
                std::string marker;
                require(!zero->get("prefix", marker), "source scalar prefix");
            }
        }
        forwarder.join();
        require(moves == (cancel ? 0u : 2u), "source MOVE count");
        const std::vector<unsigned> expected = cancel ?
            std::vector<unsigned>{0, 0} : std::vector<unsigned>{0, 7, 0, 0};
        require(databases == expected, "source reordered page/MOVE events");
        const auto after = image(zero, family_key);
        logical_image(image(source.from, family_key), after);
        require(after.pages.at(2).address == before.pages.at(2).address &&
            image(source.from, family_key).pages.at(2).address ==
                native.pages.at(2).address &&
            !image(seven, family_key).found &&
            !image(source.to, family_key).found,
            "observed MOVE copied unchanged pages or left target data");
        std::string marker;
        require(zero->get("prefix", marker) && marker == native_string_fixture("one") &&
            zero->get("suffix", marker) && marker == native_string_fixture("two"),
            "source scalar order lost");
        if (cancel) {
            require(!old->closed(), "cancelled MOVE needlessly required v4");
            for (size_t i = 0; i < records; ++i) {
                rpc_packet packet;
                require(rpc_reader::read_packet(older.reader, packet) &&
                    packet.page && packet.frame.protocol == 3 &&
                    packet.frame.operation != page_operation::family_move,
                    "cancelled MOVE changed legacy page-only encoding");
            }
        } else require(old->closed(), "v3 peer accepted MOVE downgrade");
        require(!source.publisher.pending_bytes() &&
            !source.publisher.pending_messages(), "source queue leaked");
        pair.reader->close(); older.reader->close();
    });
    acl::fiber::schedule(); controller.join();
    require(!client->pending_bytes() && !old->pending_bytes(),
        "source clients retained references");
}
static void standalone_live(bool tbb) {
    move_source source(tbb);
    publication_streams pair, older;
    auto client = attach(source, pair), old = attach(source, older);
    auto zero = memory_database(tbb, 3), seven = memory_database(tbb, 5);
    require(zero->open("") && seven->open(""), "standalone receiver open");
    seed_memory(zero, family_key, 3, family_deadline);
    auto pool = std::make_shared<database_pool>(zero, 16, [&](size_t id) {
        return id == 7 ? seven : shared_db();
    });
    cluster_client receiver("", zero, pool);
    const auto before = image(zero, family_key);
    const auto native = image(source.from, family_key);
    go[client] { client->run(); };
    go[old] { old->run(); };
    std::thread controller([&] {
        require(receiver.apply_packet(negotiate(pair, 4)), "standalone READY");
        negotiate(older, 3); settled(*client); settled(*old);
        bool moved = false;
        require(source.transfer(false, false, -1, moved) ==
            db::batch_status::ok && moved && !client->closed() &&
            !old->closed() && !client->pending_messages() &&
            source.publisher.pending_messages() == 3,
            "standalone MOVE did not queue one prepared immutable group");
        std::thread forwarder([&] { source.drain(); });
        for (unsigned i = 0; i < 3; ++i) {
            rpc_packet packet;
            require(rpc_reader::read_packet(pair.reader, packet) &&
                packet.page && packet.frame.operation == (i == 0 ?
                    page_operation::exec_begin : i == 1 ?
                    page_operation::family_move : page_operation::exec_commit)
                && receiver.apply_packet(std::move(packet)),
                "standalone source frame/order/replay");
            if (i < 2) require(image(zero, family_key) == before &&
                !image(seven, family_key).found, "standalone prefix applied");
        }
        forwarder.join();
        const auto after = image(seven, family_key);
        logical_image(image(source.to, family_key), after);
        require(after.pages == before.pages &&
            image(source.to, family_key) == native &&
            !image(zero, family_key).found &&
            !image(source.from, family_key).found && old->closed() &&
            !source.publisher.pending_bytes() &&
            !source.publisher.pending_messages(),
            "standalone ownership, old-peer rejection or budget cleanup");
        pair.reader->close(); older.reader->close();
    });
    acl::fiber::schedule(); controller.join();
    require(!client->pending_bytes() && !old->pending_bytes(),
        "standalone client references leaked");
}
int main() {
    acl::acl_cpp_init();
    for (bool grouped : {false, true}) {
        cancellation(grouped); prepare_faults(grouped);
        queue_limits(grouped);
        native_prepare_failure(grouped);
        stopped_publication(grouped);
    }
    paired_source_rejection(false); paired_source_rejection(true);
    paired_observers(); normalized_limit(false); normalized_limit(true);
    acl::fiber::acl_io_hook();
    live_source(false, false); live_source(false, true);
    standalone_live(false);
#ifdef HAS_TBB
    live_source(true, false); live_source(true, true);
    standalone_live(true);
#endif
    acl::fiber::acl_io_unlock();
    std::cout << "Prepared MOVE observation and ordered source capture "
        "passed\n";
}
