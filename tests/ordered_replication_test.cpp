#include "stdafx.h"
#include "family_workspace_support.h"
#include "rpc/exec_stream.h"
#include "rpc/move_intent.h"
#include "rpc/ordered_receiver.h"
#include "rpc/rpc_reader.h"
#include "rpc/rpc_sender.h"
#include "rpc/page_wire.h"
#include "cluster/cluster_client.h"
#include <chrono>
#include <poll.h>
#include <sys/socket.h>

struct ordered_streams {
    acl::shared_stream writer = std::make_shared<acl::socket_stream>();
    acl::shared_stream reader = std::make_shared<acl::socket_stream>();
    ordered_streams() {
        int fds[2];
        require(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0,
            "ordered socketpair");
        require(writer->open(fds[0]) && reader->open(fds[1]),
            "ordered streams");
        reader->set_rw_timeout(2);
    }
};
static std::vector<page_frame> wire(const exec_batch& batch) {
    ordered_streams stream;
    auto sent = std::async(std::launch::async, [&] {
        return exec_stream::send(stream.writer, batch, 4);
    });
    std::vector<page_frame> frames;
    for (size_t i = 0; i < batch.records(); ++i) {
        rpc_packet packet;
        require(rpc_reader::read_packet(stream.reader, packet) && packet.page,
            "ordered packet read");
        frames.push_back(std::move(packet.frame));
    }
    require(sent.get(), "ordered send");
    return frames;
}
static std::shared_ptr<const exec_batch> decode(
    const std::vector<page_frame>& frames) {
    ordered_receiver receiver;
    std::shared_ptr<const exec_batch> out;
    for (size_t i = 0; i < frames.size(); ++i) {
        const auto expected = i + 1 == frames.size() ?
            page_receiver::result::complete : page_receiver::result::pending;
        require(receiver.accept(frames[i], out) == expected,
            "ordered decode state");
        require(bool(out) == (i + 1 == frames.size()), "ordered early output");
    }
    return out;
}
static void literal() {
    family_move move;
    move.source = 2; move.target = 7; move.key.assign("k\0", 2);
    move.source_root = "A"; move.target_found = true; move.target_root = "B";
    const unsigned char expected[] = {
        0x50,0x4b,0x56,0x34, 0,0,0,16, 0,0,0,2, 0,0,0,2, 0,0,0,18,
        'k',0, 0,0,0,7, 0,0,0,1, 0,0,0,1, 0,0,0,1, 'A','B'
    };
    ordered_streams stream;
    require(move_intent::send(stream.writer, move), "literal MOVE send");
    char actual[sizeof(expected)];
    require(stream.reader->read(actual, sizeof(actual), true) ==
        sizeof(actual) && !std::memcmp(actual, expected, sizeof(actual)),
        "MOVE wire differs from literal bytes");
    family_move empty = move; empty.source_root.clear();
    empty.target_root.clear();
    auto group = exec_batch::capture_ordered({exec_entry(empty)});
    require(group && group->records() == 3 && group->bytes() == 94,
        "MOVE empty-root measurement");
    auto decoded = decode(wire(*group));
    require(decoded->entries()[0].move()->target_found &&
        decoded->entries()[0].move()->target_root.empty(),
        "found empty root became absent");
    empty.target_found = false;
    group = exec_batch::capture_ordered({exec_entry(empty)});
    require(group && !decode(wire(*group))->entries()[0].move()->target_found,
        "absent root became present");
}
static void cycles_over_rpc(bool tbb) {
    workspace_fixture files(tbb);
    auto moves = files.cycle();
    std::vector<exec_entry> entries;
    for (const auto& move : moves) entries.emplace_back(move);
    auto group = exec_batch::capture_ordered(std::move(entries));
    require(group && group->protocol() == 4 && group->has_moves(),
        "ordered cycle capture");
    moves[0].source_root = "caller changed";
    const auto frames = wire(*group);
    auto pool = std::shared_ptr<database_pool>(files.pool.get(),
        [](database_pool*) {});
    cluster_client receiver("", files.databases[0], pool);
    const auto ready = std::make_shared<kv_message>("", "",
        message_oper_ready, 0, 4);
    require(receiver.apply(ready), "ordered READY");
    for (size_t i = 0; i < frames.size(); ++i) {
        rpc_packet packet; packet.page = true; packet.frame = frames[i];
        require(receiver.apply_packet(std::move(packet)), "ordered RPC cycle");
        if (i + 1 != frames.size()) files.unchanged();
    }
    files.complete();
}
static void deltas_over_rpc(bool tbb) {
    workspace_fixture files(tbb);
    family_move first;
    first.source = 0; first.target = 7; first.key = family_key;
    first.source_root = files.before[0].root;
    family_move second;
    second.source = 7; second.target = 15; second.key = family_key;
    second.source_root = encoded_root(4, family_deadline);
    auto group = exec_batch::capture_ordered({
        exec_entry(workspace_delta(0, db::update_action::keep, "",
            {{family_key, 1, page_bytes(99), db::update_action::write}})),
        exec_entry(workspace_delta(7, db::update_action::erase, "")),
        exec_entry(first),
        exec_entry(workspace_delta(7, db::update_action::write,
            second.source_root,
            {{family_key, 4, page_bytes(77), db::update_action::write}})),
        exec_entry(workspace_delta(15, db::update_action::erase, "")),
        exec_entry(second),
        exec_entry(workspace_delta(15, db::update_action::write,
            encoded_root(3, -1),
            {{family_key, 4, "", db::update_action::erase}}))});
    require(group && group->touches(0) && group->touches(15) &&
        !group->touches(4), "ordered membership");
    auto restored = decode(wire(*group)); files.unchanged();
    require(restored->apply(*files.pool) == db::batch_status::ok,
        "ordered delta apply");
    const auto result = image(files.databases[2], family_key);
    require(result.root == encoded_root(3, -1) && result.pages.size() == 3 &&
        result.pages.at(2) == files.before[0].pages.at(2) &&
        result.pages.at(3) == files.before[0].pages.at(3) &&
        !image(files.databases[0], family_key).found &&
        !image(files.databases[1], family_key).found && !files.events.calls,
        "ordered replay lost unchanged native page identities");
}
static void invalid_and_admission() {
    workspace_fixture files(false);
    auto moves = files.cycle();
    auto input = exec_batch::capture_ordered({exec_entry(moves[0])});
    const auto frames = wire(*input);
    for (unsigned fault = 0; fault < 10; ++fault) {
        auto bad = frames;
        if (fault == 0) bad[0].protocol = 3;
        if (fault == 1) page_wire::put32(&bad[0].value[0], UINT32_MAX);
        if (fault == 2) page_wire::put64(&bad[0].value[8], 55);
        if (fault == 3) page_wire::put32(&bad[1].value[4], 2);
        if (fault == 4) page_wire::put32(&bad[1].value[8], UINT32_MAX);
        if (fault == 5) page_wire::put32(&bad[1].value[0], 65536);
        if (fault == 6) bad[1].database = moves[0].target;
        if (fault == 7) bad[1].id = 1;
        if (fault == 8) bad.back().database = 1;
        if (fault == 9) bad[1].value += 'x';
        ordered_receiver decoder;
        std::shared_ptr<const exec_batch> out;
        bool rejected = false;
        for (auto& frame : bad) {
            if (decoder.accept(std::move(frame), out) ==
                page_receiver::result::error) { rejected = true; break; }
        }
        require(rejected && !out && !decoder.active() &&
            decoder.accept(frames[0], out) == page_receiver::result::error,
            "ordered malformed input not poisoned");
        files.unchanged();
    }
    ordered_receiver decoder;
    std::shared_ptr<const exec_batch> out;
    require(decoder.accept(frames[0], out) == page_receiver::result::pending,
        "admission header");
    auto progress = std::async(std::launch::async, [] {
        execution_guard writer(true); return true;
    });
    const bool ready = progress.wait_for(std::chrono::milliseconds(250)) ==
        std::future_status::ready;
    decoder.reset(); require(progress.get() && ready,
        "incomplete ordered group held execution admission");
    moves[0].source_root += 'x';
    input = exec_batch::capture_ordered({exec_entry(moves[0])});
    require(decode(wire(*input))->apply(*files.pool) ==
        db::batch_status::aborted, "wrong prefix accepted");
    files.unchanged();
}
static void old_peers_and_faults() {
    workspace_fixture files(false);
    auto group = exec_batch::capture_ordered({exec_entry(files.cycle()[0])});
    kv_message message(group), normal("plain", "value", message_oper_set);
    for (unsigned version : {0, 1, 2, 3}) {
        ordered_streams stream;
        const bool sent = version ? rpc_sender::send_versioned(stream.writer,
            {&normal, &message}, version) :
            rpc_sender::send(stream.writer, {&normal, &message});
        pollfd fd = {stream.reader->sock_handle(), POLLIN, 0};
        require(!sent && ::poll(&fd, 1, 0) == 0,
            "old peer received ordered prefix");
    }
    const auto frames = wire(*group);
    unsigned failed = 0, complete = 0;
    for (long limit = 0; limit < 64; ++limit) {
        ordered_receiver decoder;
        std::shared_ptr<const exec_batch> out;
        bool error = false;
        family_budget = limit;
        try {
            for (const auto& frame : frames) if (decoder.accept(frame, out) ==
                page_receiver::result::error) { error = true; break; }
        } catch (const std::bad_alloc&) { error = true; }
        family_budget = -1;
        if (error) { ++failed; require(!out, "OOM published ordered group"); }
        else { ++complete; require(out != nullptr, "ordered OOM no output"); }
        files.unchanged();
    }
    require(failed && complete, "ordered allocation coverage missing");
}
static void large_family(bool tbb) {
    for (size_t count : {size_t(1), size_t(64), size_t(4097)}) {
        memory_fixture files(tbb, true, false, count);
        const auto before = image(files.from, family_key);
        family_move move;
        move.source = 0; move.target = 1; move.key = family_key;
        move.source_root = before.root;
        control(files.from).reset(); control(files.to).reset();
        auto group = exec_batch::capture_ordered({exec_entry(move)});
        require(group && group->bytes() < 1024 && group->records() == 3,
            "wire intent expanded a native collection");
        auto restored = decode(wire(*group));
        require(restored->apply(*files.pool) == db::batch_status::ok &&
            !control(files.from).visits && !control(files.from).page_reads &&
            !control(files.from).page_writes && !control(files.to).visits &&
            !control(files.to).page_reads && !control(files.to).page_writes,
            "wire MOVE enumerated or rewrote unchanged pages");
        require(image(files.to, family_key) == before &&
            !image(files.from, family_key).found,
            "wire MOVE copied or lost native allocations");
    }
}
static void page_only_compatibility() {
    const auto pages = workspace_delta(0, db::update_action::write,
        encoded_root(1, -1), {{family_key, 1, page_bytes(1),
            db::update_action::write}}, true);
    const auto original = exec_batch::capture({pages});
    const auto ordered = exec_batch::capture_ordered({exec_entry(pages)});
    require(ordered && ordered->protocol() == 3 &&
        ordered->entries().empty() && !ordered->has_moves(),
        "page-only capture needlessly requires v4");
    ordered_streams left, right;
    auto first = std::async(std::launch::async, [&] {
        return exec_stream::send(left.writer, *original);
    });
    auto second = std::async(std::launch::async, [&] {
        return exec_stream::send(right.writer, *ordered);
    });
    std::string a(original->bytes(), '\0'), b(ordered->bytes(), '\0');
    require(left.reader->read(&a[0], a.size(), true) ==
        static_cast<int>(a.size()) && right.reader->read(&b[0], b.size(),
            true) == static_cast<int>(b.size()) && first.get() &&
            second.get() && a == b, "v3 byte encoding changed");
    auto restored = decode(wire(*ordered));
    require(restored->protocol() == 3 && !restored->has_moves(),
        "v4 page-only replay lost legacy capability");
}
int main() {
    acl::acl_cpp_init(); literal(); invalid_and_admission();
    old_peers_and_faults(); cycles_over_rpc(false); deltas_over_rpc(false);
    large_family(false); page_only_compatibility();
#ifdef HAS_TBB
    cycles_over_rpc(true); deltas_over_rpc(true); large_family(true);
#endif
    std::cout << "PKV4 mixed ordered replay and native ownership passed\n";
}
