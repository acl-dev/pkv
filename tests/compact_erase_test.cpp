#include "stdafx.h"
#include "rpc/page_stream.h"
#include "rpc/page_wire.h"
#include "rpc/rpc_reader.h"
#include "rpc/rpc_sender.h"
#include "rpc/exec_stream.h"
#include "rpc/ordered_receiver.h"
#include "db/mdb/mdb.h"
#include "common/replica_budget.h"
#include <future>
#include <poll.h>
#include <sys/socket.h>
#include <cstdlib>
#include <iostream>
#include <new>

static thread_local long budget = -1;
void* operator new(size_t n) {
    if (!budget) throw std::bad_alloc();
    if (budget > 0) --budget;
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
#if __cplusplus >= 201402L
void operator delete(void* p, size_t) noexcept { std::free(p); }
void operator delete[](void* p, size_t) noexcept { std::free(p); }
#endif
using namespace pkv;
using action = db::update_action;
using result = page_receiver::result;
static void require(bool ok, const char* text) {
    if (!ok) { std::cerr << text << '\n'; std::exit(1); }
}
struct streams {
    acl::shared_stream writer = std::make_shared<acl::socket_stream>();
    acl::shared_stream reader = std::make_shared<acl::socket_stream>();
    streams() {
        int fd[2];
        require(!socketpair(AF_UNIX, SOCK_STREAM, 0, fd), "socketpair");
        require(writer->open(fd[0]) && reader->open(fd[1]), "streams");
        reader->set_rw_timeout(3);
    }
};
struct fixture {
    std::vector<db::batch_entry> roots;
    std::vector<page_change> pages;
    explicit fixture(size_t count, const std::string& key = "owner") {
        roots.emplace_back(key, true, "new");
        roots.back().action = action::write;
        pages.reserve(count + 1);
        pages.emplace_back(key, 1, "kept", action::write);
        for (size_t i = 0; i < count; ++i)
            pages.emplace_back(key, 3 + 2 * i, "", action::erase);
    }
};
static std::vector<page_frame> wire(const page_batch& batch) {
    streams pair;
    auto sent = std::async(std::launch::async, [&] {
        return page_stream::send(pair.writer, batch, 5);
    });
    std::vector<page_frame> frames;
    for (size_t i = 0; i < batch.size().records; ++i) {
        rpc_packet packet;
        require(rpc_reader::read_packet(pair.reader, packet) && packet.page,
            "compact wire read");
        frames.push_back(std::move(packet.frame));
    }
    require(sent.get(), "compact wire send"); return frames;
}
static std::shared_ptr<const page_batch> decode(
    const std::vector<page_frame>& frames) {
    page_receiver receiver;
    std::shared_ptr<const page_batch> out;
    for (size_t i = 0; i < frames.size(); ++i) {
        require(receiver.accept(frames[i], out) ==
            (i + 1 == frames.size() ? result::complete : result::pending),
            "compact decode state");
        require(bool(out) == (i + 1 == frames.size()), "early publication");
    }
    return out;
}
static void measurement() {
    fixture input(66000);
    auto batch = page_batch::capture(0, input.roots, input.pages);
    require(batch && batch->required_protocol() == 5 &&
        batch->size().pages == 66001 && batch->size().actions == 66004 &&
        batch->size().records == 69, "group plan counts");
    const auto frames = wire(*batch);
    auto decoded = decode(frames);
    require(decoded->pages().size() == input.pages.size() &&
        decoded->size().bytes == batch->size().bytes &&
        decoded->size().expanded_bytes == batch->size().expanded_bytes,
        "decoded plan size");
    for (size_t i = 0; i < input.pages.size(); ++i) {
        const auto& x = input.pages[i]; const auto& y = decoded->pages()[i];
        require(x.owner == y.owner && x.id == y.id && x.value == y.value &&
            x.action == y.action, "decoded native action");
    }
    streams old;
    auto* message = new kv_message(batch);
    require(!rpc_sender::send_versioned(old.writer, {message}, 4),
        "old peer accepted compact message");
    message->unrefer();
    pollfd fd{old.reader->sock_handle(), POLLIN, 0};
    require(::poll(&fd, 1, 5) == 0, "old peer received partial message");
    replica_budget pending;
    const auto& size = batch->size();
    for (unsigned i = 0; i < 3; ++i)
        require(pending.reserve_encoded(size.bytes, size.records,
            size.expanded_bytes, size.actions), "expanded queue admission");
    require(!pending.reserve_encoded(size.bytes, size.records,
        size.expanded_bytes, size.actions), "queue ignored native action cap");
    for (unsigned i = 0; i < 3; ++i)
        pending.release_encoded(size.bytes, size.records,
            size.expanded_bytes, size.actions);
    require(!pending.bytes() && !pending.messages() &&
        !pending.expanded_bytes() && !pending.expanded_records(),
        "expanded credits leaked");
    auto database = db::create_mdb(4);
    require(database && database->open(""), "native destination");
    require(database->transact_page("owner", [&](page_transaction& tx) {
        auto* root = tx.roots().find("owner");
        root->action = action::write; root->value = "old";
        for (const auto& page : input.pages)
            if (!tx.put("owner", page.id, "old")) return false;
        return true;
    }) == db::batch_status::ok, "native fixture");
    std::string value;
    require(database->get("owner", value) && value == "old",
        "decode modified destination");
    require(decoded->apply(*database) == db::batch_status::ok,
        "large compact native apply");
    require(database->get("owner", value) && value == "new", "new root");
    require(database->transact_page("owner", [&](page_transaction& tx) {
        if (tx.lookup("owner", 1, value) != db::lookup_status::found ||
            value != "kept") return false;
        for (size_t i = 1; i < input.pages.size(); ++i)
            if (tx.lookup("owner", input.pages[i].id, value) !=
                db::lookup_status::missing) return false;
        return true;
    }) == db::batch_status::ok, "native erased set");
}
static std::vector<page_frame> small_frames() {
    const std::string key(64, 'k');
    fixture input(3, key); page_batch_size size;
    require(page_batch::measure_compact(0, input.roots, input.pages, size),
        "small compact plan");
    page_frame begin; begin.protocol = 5;
    begin.value.assign(16, '\0');
    page_wire::put32(&begin.value[0], 1);
    page_wire::put32(&begin.value[4], 4);
    page_wire::put64(&begin.value[8], size.bytes);
    page_frame root; root.protocol = 5; root.key = key;
    root.operation = page_operation::root_write; root.value = "new";
    page_frame page = root; page.operation = page_operation::page_write;
    page.id = 1; page.value = "kept";
    page_frame group = root; group.operation = page_operation::page_erase_group;
    group.value.assign(24, '\0');
    for (size_t i = 0; i < 3; ++i)
        page_wire::put64(&group.value[i * 8], 3 + 2 * i);
    page_frame commit; commit.protocol = 5;
    commit.operation = page_operation::commit;
    return {begin, root, page, group, commit};
}
static void invalid_and_faults() {
    const auto frames = small_frames();
    require(decode(frames)->pages().size() == 4, "small grouped decode");
    for (unsigned mode = 0; mode < 8; ++mode) {
        auto changed = frames;
        auto& group = changed[3];
        if (mode == 0) group.value.pop_back();
        if (mode == 1) page_wire::put64(&group.value[0], 0);
        if (mode == 2) page_wire::put64(&group.value[8], 3);
        if (mode == 3) group.protocol = 4;
        if (mode == 4) group.key = "unknown";
        if (mode == 5) group.value.resize(8);
        if (mode == 6) group.value.resize(8 * 1025);
        if (mode == 7) page_wire::put32(&changed[0].value[4], 2);
        page_receiver receiver; std::shared_ptr<const page_batch> out;
        bool rejected = false;
        for (auto& frame : changed) {
            if (receiver.accept(std::move(frame), out) == result::error) {
                rejected = true; break;
            }
            require(!out, "bad group published");
        }
        require(rejected && !out, "bad group accepted");
    }
    bool succeeded = false; size_t failures = 0;
    for (long available = 0; available < 32; ++available) {
        auto copied = frames; page_receiver receiver;
        std::shared_ptr<const page_batch> out;
        budget = available; result state = result::pending;
        for (auto& frame : copied) {
            state = receiver.accept(std::move(frame), out);
            if (state == result::error) break;
        }
        budget = -1;
        if (state == result::complete) { succeeded = true; break; }
        require(!out && state == result::error, "OOM published group");
        ++failures;
    }
    require(succeeded && failures, "decode fault sweep incomplete");
    page_receiver limited; std::shared_ptr<const page_batch> out;
    require(limited.accept(frames[0], out, 1, 100) == result::error && !out,
        "receiver ignored storage budget");
    limited.reset();
    require(limited.accept(frames[0], out,
        page_batch::max_expanded_bytes, 5) == result::error,
        "receiver ignored action budget");
}
static void execution_groups() {
    auto small = decode(small_frames());
    std::vector<db::batch_entry> roots;
    roots.emplace_back("aux", false, "value");
    roots.back().action = action::write;
    auto normal = page_batch::capture(1, roots, {});
    require(normal && normal->required_protocol() == 2, "legacy small plan");
    family_move move;
    move.source = 1; move.target = 2; move.key = "move";
    move.source_root = "source";
    auto group = exec_batch::capture_ordered({exec_entry(normal),
        exec_entry(small), exec_entry(move)});
    require(group && group->protocol() == 5, "mixed group minimum protocol");
    streams pair;
    auto sent = std::async(std::launch::async, [&] {
        return exec_stream::send(pair.writer, *group, 5);
    });
    ordered_receiver receiver; std::shared_ptr<const exec_batch> out;
    bool old_frame = false, grouped = false;
    for (size_t i = 0; i < group->records(); ++i) {
        rpc_packet packet;
        require(rpc_reader::read_packet(pair.reader, packet) && packet.page,
            "mixed EXEC read");
        old_frame = old_frame || packet.frame.protocol == 4;
        grouped = grouped || packet.frame.operation ==
            page_operation::page_erase_group;
        require(receiver.accept(std::move(packet.frame), out) ==
            (i + 1 == group->records() ? result::complete : result::pending),
            "mixed EXEC state");
        require(bool(out) == (i + 1 == group->records()),
            "mixed EXEC early output");
    }
    require(sent.get() && old_frame && grouped && out->protocol() == 5 &&
        out->entries().size() == 3 && out->has_moves() &&
        out->expanded_bytes() == group->expanded_bytes() &&
        out->actions() == group->actions(), "mixed EXEC roundtrip");

    std::vector<db::batch_entry> missing;
    missing.emplace_back("missing", true, "");
    std::vector<page_change> write;
    write.emplace_back("missing", 1, "bad", action::write);
    auto bad = page_batch::capture(0, missing, write);
    auto failed = exec_batch::capture({small, bad});
    auto database = db::create_mdb(4);
    require(database && database->open("") && failed,
        "late failure fixture");
    require(failed->apply(*database) != db::batch_status::ok,
        "missing-owner group applied");
    std::string value;
    require(!database->get(std::string(64, 'k'), value),
        "late failure published first batch");

    fixture large(66000);
    auto batch = page_batch::capture(0, large.roots, large.pages);
    require(bool(exec_batch::capture({batch, batch, batch})) &&
        !exec_batch::capture({batch, batch, batch, batch}),
        "EXEC ignored aggregate expanded actions");
    const auto frames = wire(*batch);
    page_frame begin; begin.protocol = 5;
    begin.operation = page_operation::exec_begin;
    begin.value.assign(16, '\0');
    page_wire::put32(&begin.value[0], 4);
    page_wire::put32(&begin.value[4],
        static_cast<uint32_t>(2 + 4 * batch->size().records));
    page_wire::put64(&begin.value[8], 56 + 4 * batch->size().bytes);
    receiver.reset(); out.reset();
    require(receiver.accept(begin, out) == result::pending,
        "aggregate EXEC begin");
    for (unsigned i = 0; i < 3; ++i)
        for (const auto& frame : frames)
            require(receiver.accept(frame, out) == result::pending && !out,
                "aggregate EXEC prefix");
    require(receiver.accept(frames[0], out) == result::error && !out,
        "aggregate EXEC allocated beyond action limit");
}
int main() {
    acl::acl_cpp_init();
    measurement(); invalid_and_faults(); execution_groups();
    std::cout << "Compact erasures: wire, bounds, OOM, native apply passed\n";
}
