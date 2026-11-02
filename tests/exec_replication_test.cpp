#include "stdafx.h"
#include "rpc/exec_stream.h"
#include "rpc/rpc_reader.h"
#include "rpc/rpc_sender.h"
#include "rpc/page_wire.h"
#include "cluster/cluster_client.h"
#include "db/database_pool.h"
#include "db/mdb/mdb.h"
#include "slave/replica_scope.h"
#include "slave/slave_watcher.h"
#include <cstdlib>
#include <iostream>
#include <exception>
#include <new>
#include <poll.h>
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
void operator delete[](void* p) noexcept { std::free(p); }
#if __cplusplus >= 201402L
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
#endif

using namespace pkv;
using action = db::update_action;
using result = exec_receiver::result;
static void require(bool ok, const char* message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}
struct streams {
    acl::shared_stream writer = std::make_shared<acl::socket_stream>();
    acl::shared_stream reader = std::make_shared<acl::socket_stream>();
    streams() {
        int sockets[2];
        require(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0,
            "socketpair");
        require(writer->open(sockets[0]) && reader->open(sockets[1]),
            "streams");
        reader->set_rw_timeout(2);
    }
};
class counted_db : public mdb {
public:
    counted_db() : mdb(4) {}
    unsigned commits = 0;
    bool fail_observer = false, fail_page_observer = false;
protected:
    batch_status dbapply_pages(const std::vector<batch_entry>& roots,
        const std::vector<page_change>& pages) override {
        ++commits;
        const auto status = mdb::dbapply_pages(roots, pages);
        if (status == batch_status::ok && fail_page_observer) budget = 0;
        return status;
    }
    bool dbset(const std::string& key, const std::string& value) override {
        const bool ok = mdb::dbset(key, value);
        if (ok && fail_observer) budget = 0;
        return ok;
    }
};
static std::shared_ptr<counted_db> memory(db_watchers* watchers = nullptr) {
    auto database = std::make_shared<counted_db>();
    require(database->open("", watchers), "memory open"); return database;
}
static std::shared_ptr<const page_batch> pages(unsigned database,
    const std::string& key, const std::string& value = "root") {
    std::vector<db::batch_entry> roots;
    roots.emplace_back(key, false, value);
    roots.back().action = action::write;
    roots.back().clear_pages = true;
    return page_batch::capture(database, roots,
        {{key, 1, "bounded-page", action::write}});
}
static std::shared_ptr<const exec_batch> group(unsigned right_db = 0) {
    auto value = exec_batch::capture({pages(0, "left"),
        pages(right_db, std::string("right\0key", 9))});
    require(value != nullptr, "EXEC capture"); return value;
}
static std::vector<page_frame> frames(const exec_batch& value) {
    streams pair;
    require(exec_stream::send(pair.writer, value), "EXEC stream send");
    std::vector<page_frame> out;
    for (size_t i = 0; i < value.records(); ++i) {
        rpc_packet packet;
        require(rpc_reader::read_packet(pair.reader, packet) && packet.page,
            "EXEC stream read");
        out.push_back(std::move(packet.frame));
    }
    return out;
}
static void absent(shared_db database) {
    std::string value;
    require(!database->get("left", value) &&
        !database->get(std::string("right\0key", 9), value),
        "EXEC prefix became visible");
}
static void complete(shared_db database) {
    std::string value;
    require(database->get("left", value) && value == "root" &&
        database->get(std::string("right\0key", 9), value) && value == "root",
        "EXEC metadata missing");
}
static void decode_and_apply() {
    auto input = group();
    const auto wire = frames(*input);
    require(wire.front().protocol == 3 &&
        wire.front().operation == page_operation::exec_begin &&
        wire.back().operation == page_operation::exec_commit,
        "EXEC control framing");
    exec_receiver decoder;
    std::shared_ptr<const exec_batch> out;
    auto destination = memory();
    for (size_t i = 0; i < wire.size(); ++i) {
        require(decoder.accept(wire[i], out) ==
            (i + 1 == wire.size() ? result::complete : result::pending),
            "EXEC decode state");
        absent(destination);
        require(bool(out) == (i + 1 == wire.size()), "premature EXEC output");
    }
    require(out && out->bytes() == input->bytes() &&
        out->records() == input->records() && out->apply(*destination) ==
            db::batch_status::ok && destination->commits == 1,
        "EXEC did not commit once");
    complete(destination);
    destination = memory();
    cluster_client receiver("", destination);
    require(receiver.apply(std::make_shared<kv_message>("", "",
        message_oper_ready, 0, 3)), "EXEC READY");
    for (size_t i = 0; i < wire.size(); ++i) {
        rpc_packet packet; packet.page = true; packet.frame = wire[i];
        require(receiver.apply_packet(std::move(packet)), "live EXEC apply");
        if (i + 1 != wire.size()) absent(destination);
    }
    require(destination->commits == 1, "live EXEC committed a prefix");
    complete(destination);
}
static void mixed_and_old_peers() {
    auto input = group(7);
    unsigned database = 99;
    auto destination = memory();
    require(!input->database(database) && database == 99 &&
        input->apply(*destination) == db::batch_status::unsupported &&
        destination->commits == 0, "cross-DB EXEC applied a prefix");
    cluster_client receiver("", destination);
    require(receiver.apply(std::make_shared<kv_message>("", "",
        message_oper_ready, 0, 3)), "mixed READY");
    const auto wire = frames(*input);
    for (size_t i = 0; i < wire.size(); ++i) {
        rpc_packet packet; packet.page = true; packet.frame = wire[i];
        require(receiver.apply_packet(std::move(packet)) ==
            (i + 1 != wire.size()), "mixed DB preflight");
        absent(destination);
    }
    require(destination->commits == 0, "mixed DB called storage");
    for (unsigned version : {0, 1, 2}) {
        cluster_client old_receiver("", destination);
        if (version) require(old_receiver.apply(std::make_shared<kv_message>(
            "", "", message_oper_ready, 0, version)), "old READY");
        rpc_packet packet; packet.page = true; packet.frame = wire.front();
        require(!old_receiver.apply_packet(std::move(packet)),
            "EXEC accepted before v3 negotiation");
        absent(destination);
        streams pair;
        kv_message normal("normal", "value", message_oper_set);
        kv_message message(input);
        require(!(version ? rpc_sender::send_versioned(pair.writer,
            {&normal, &message}, version) : rpc_sender::send(pair.writer,
                {&normal, &message})), "EXEC downgraded to an old stream");
        pollfd fd{pair.reader->sock_handle(), POLLIN, 0};
        require(::poll(&fd, 1, 5) == 0, "old stream received a batch prefix");
    }
}
static void rejection(std::vector<page_frame> wire) {
    exec_receiver decoder;
    std::shared_ptr<const exec_batch> out;
    bool rejected = false;
    for (const auto& frame : wire) {
        const auto status = decoder.accept(frame, out);
        require(!out, "malformed EXEC returned data");
        if (status == result::error) { rejected = true; break; }
    }
    require(rejected, "malformed EXEC accepted");
    const auto valid = frames(*group());
    require(decoder.accept(valid.front(), out) == result::error,
        "EXEC error was not sticky");
    decoder.reset();
    for (size_t i = 0; i < valid.size(); ++i)
        require(decoder.accept(valid[i], out) ==
            (i + 1 == valid.size() ? result::complete : result::pending),
            "EXEC reset failed");
}
static void malformed() {
    const auto valid = frames(*group());
    auto wire = valid; wire[0].protocol = 2; rejection(wire);
    wire = valid; wire[0].database = 1; rejection(wire);
    wire = valid; wire[0].key = "bad"; rejection(wire);
    wire = valid; page_wire::put32(&wire[0].value[0], UINT32_MAX);
    rejection(wire);
    wire = valid; page_wire::put32(&wire[0].value[4], 2); rejection(wire);
    wire = valid; page_wire::put64(&wire[0].value[8], UINT64_MAX);
    rejection(wire);
    wire = valid; wire[1].protocol = 2; rejection(wire);
    wire = valid;
    page_wire::put64(&wire[1].value[8], page_batch::max_bytes);
    rejection(wire);
    wire = valid; wire[2] = valid.back(); rejection(wire);
    wire = valid; wire[3].key = "undeclared"; rejection(wire);
    wire = valid; wire[1] = valid[0]; rejection(wire);
    wire = valid; wire.back().id = 1; rejection(wire);
}
static void truncation_and_intrusion() {
    const auto wire = frames(*group());
    for (size_t count = 1; count < wire.size(); ++count) {
        auto destination = memory(); cluster_client receiver("", destination);
        require(receiver.apply(std::make_shared<kv_message>("", "",
            message_oper_ready, 0, 3)), "prefix READY");
        for (size_t i = 0; i < count; ++i) {
            rpc_packet packet; packet.page = true; packet.frame = wire[i];
            require(receiver.apply_packet(std::move(packet)), "EXEC prefix");
        }
        absent(destination);
        require(!receiver.apply(std::make_shared<kv_message>("left",
            "intrusion", message_oper_set, 0, 3)), "normal frame inside EXEC");
        absent(destination);
    }
}
static void allocation_failures() {
    const auto valid = frames(*group());
    bool success = false; unsigned failed = 0;
    for (long allocation = 0; allocation < 200 && !success; ++allocation) {
        auto wire = valid;
        exec_receiver decoder;
        std::shared_ptr<const exec_batch> out;
        budget = allocation;
        for (auto& frame : wire)
            if (decoder.accept(std::move(frame), out) == result::error) break;
        budget = -1;
        if (out) success = true;
        else ++failed;
    }
    require(success && failed >= 5, "EXEC receiver allocation sweep");
}
static void publication(unsigned failure) {
    // Two raw root events become 82 and 81 byte native page batches; the
    // outer EXEC adds 56 bytes and two records. No data is queued mid-scope.
    slave_watcher publisher(failure == 1 ? 218 : 219,
        failure == 2 ? 7 : 8);
    db_watchers observers; observers.add_watcher(&publisher);
    auto source = memory(&observers);
    streams pair;
    auto client = std::make_shared<slave_client>(pair.writer);
    publisher.add_client(client);
    const std::string second = failure == 4 ? std::string(16385, 'x') :
        "beta";
    try {
        replica_scope capture(&publisher);
        require(source->set("a", "alpha"), "EXEC first root");
        require(publisher.pending_bytes() == 138 &&
            publisher.pending_messages() == 5, "EXEC first charge");
        if (failure == 7) publisher.stop();
        source->fail_observer = failure == 3;
        source->fail_page_observer = failure == 5;
        const bool ok = failure == 5 ? source->transact_pages({"b"},
            [](page_transaction& tx) {
                auto& root = *tx.roots().find("b");
                root.value = "beta"; root.action = action::write;
                return true;
            }) == db::batch_status::ok : source->set("b", second);
        budget = -1;
        source->fail_observer = source->fail_page_observer = false;
        require(ok && client->pending_messages() == 0,
            "EXEC root failed or prefix forwarded");
        if (!failure) require(publisher.pending_bytes() == 219 &&
            publisher.pending_messages() == 8, "EXEC full charge");
        if (failure == 6) budget = 0;
        if (failure == 8) throw 8;
        if (failure == 9) {
            try { replica_scope nested(&publisher); throw 9; }
            catch (int) {}
        }
    } catch (int) {}
    budget = -1;
    std::string value;
    require(source->get("b", value) && value == second, "source write lost");
    if (failure) require(client->closed() && publisher.pending_bytes() == 0 &&
        publisher.pending_messages() == 0, "EXEC failure retained a prefix");
    else require(publisher.pending_bytes() == 219 &&
        publisher.pending_messages() == 8, "EXEC queue item charge");
    publisher.stop(); publisher.run();
    require(publisher.pending_bytes() == 0 &&
        publisher.pending_messages() == 0 &&
        client->closed() && client->pending_messages() == 0,
        "EXEC publication leaked charges or downgraded an old subscriber");
}
static void mailbox_failure() {
    fiber_mailbox<int> mailbox;
    int first = 1, second = 2;
    require(mailbox.push(&first), "mailbox seed");
    budget = 0;
    const bool accepted = mailbox.push(&second);
    mailbox.close();
    bool closed = false;
    auto retained = mailbox.pop(0, &closed);
    auto empty = mailbox.pop(0);
    budget = -1;
    require(!accepted && closed && retained == &first && !empty,
        "mailbox failure locked queue, lost ownership or allocated on close");
}
static void readonly_budget() {
    slave_watcher publisher(1, 1);
    db_watchers observers; observers.add_watcher(&publisher);
    auto source = memory(&observers);
    require(source->set("old", "value"), "read-only seed");
    streams pair;
    auto client = std::make_shared<slave_client>(pair.writer);
    publisher.add_client(client);
    {
        replica_scope capture(&publisher);
        std::string value;
        require(source->get("old", value) && value == "value", "scope read");
    }
    require(publisher.pending_bytes() == 0 &&
        publisher.pending_messages() == 0 && !client->closed(),
        "read-only EXEC reserved unused controls or disconnected a peer");
    publisher.stop(); publisher.run(); client->disconnect();
}
static void legacy_database_filter() {
    slave_watcher publisher;
    db_watchers observers(7); observers.add_watcher(&publisher);
    auto source = memory(&observers);
    streams pair;
    auto client = std::make_shared<slave_client>(pair.writer);
    publisher.add_client(client);
    {
        replica_scope capture(&publisher);
        require(source->set("a", "alpha") && source->set("b", "beta"),
            "DB seven EXEC roots");
    }
    publisher.stop(); publisher.run();
    require(!client->closed() && client->pending_messages() == 0 &&
        publisher.pending_bytes() == 0,
        "unrelated EXEC broke legacy DB filter");
    pollfd fd{pair.reader->sock_handle(), POLLIN, 0};
    require(::poll(&fd, 1, 5) == 0, "DB seven EXEC leaked to legacy DB zero");
    client->disconnect();
}
static void limits() {
    require(!exec_batch::capture({}) && !exec_batch::capture({nullptr}),
        "empty/null EXEC captured");
    std::vector<std::shared_ptr<const page_batch>> input;
    auto small = pages(0, "k");
    for (size_t i = 0; i < page_batch::max_records / 4; ++i)
        input.push_back(small);
    require(!exec_batch::capture(std::move(input)), "EXEC record cap ignored");
    auto large = pages(0, "k", std::string(16384, 'x'));
    input.assign(5000, large);
    require(!exec_batch::capture(std::move(input)), "EXEC byte cap ignored");
}
int main(int argc, char** argv) {
    acl::acl_cpp_init();
    if (argc == 2) {
        publication(std::stoul(argv[1])); return 0;
    }
    decode_and_apply(); mixed_and_old_peers(); malformed();
    truncation_and_intrusion(); allocation_failures(); limits();
    mailbox_failure(); readonly_budget(); legacy_database_filter();
    for (unsigned failure = 0; failure < 10; ++failure) publication(failure);
    std::cout << "EXEC framing, atomic replay and publication tests passed\n";
}
