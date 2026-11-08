#include "stdafx.h"
#include "rpc/page_batch.h"
#include "rpc/page_stream.h"
#include "rpc/page_wire.h"
#include "rpc/rpc_reader.h"
#include "cluster/cluster_client.h"
#include "db/db_watcher.h"
#include "db/mdb/mdb.h"
#include "db/list_store.h"
#include "db/database_pool.h"
#include "coder/list_page.h"
#include <sys/socket.h>
#include <unistd.h>
#include <cstdlib>
#include <iostream>
#include <new>
#include <thread>

static thread_local long allocation_budget = -1;
void* operator new(std::size_t size) {
    if (allocation_budget == 0) {
        throw std::bad_alloc();
    }
    if (allocation_budget > 0) {
        --allocation_budget;
    }
    if (void* pointer = std::malloc(size ? size : 1)) {
        return pointer;
    }
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
#if __cplusplus >= 201402L
void operator delete(void* pointer, std::size_t) noexcept {
    std::free(pointer);
}
void operator delete[](void* pointer, std::size_t) noexcept {
    std::free(pointer);
}
#endif

using namespace pkv;
using action = db::update_action;
using result = page_receiver::result;

static void require(bool ok, const char* reason) {
    if (!ok) {
        std::cerr << reason << '\n';
        std::exit(1);
    }
}

struct streams {
    acl::shared_stream writer = std::make_shared<acl::socket_stream>();
    acl::shared_stream reader = std::make_shared<acl::socket_stream>();
    streams() {
        int sockets[2];
        require(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0,
            "socketpair");
        require(writer->open(sockets[0]) && reader->open(sockets[1]),
            "open streams");
        writer->set_rw_timeout(2);
        reader->set_rw_timeout(2);
    }
};

static const std::string binary("binary\0key", 10);

static std::vector<db::batch_entry> roots() {
    std::vector<db::batch_entry> entries;
    entries.emplace_back("", true, "root-a");
    entries.back().action = action::write;
    entries.emplace_back(binary, true, "ignored");
    entries.emplace_back("zzz-untouched", true, "ignored");
    return entries;
}

static std::vector<page_change> pages() {
    return {{"", 1, "one", action::write}, {"", 2, "", action::erase},
        {binary, UINT64_MAX, "", action::write}};
}

static std::shared_ptr<const page_batch> fixture() {
    auto batch = page_batch::capture(7, roots(), pages());
    require(batch && batch->size().bytes == 209 &&
        batch->size().records == 7 && batch->size().roots == 2 &&
        batch->size().pages == 3, "batch size/filtering");
    return batch;
}

static std::vector<page_frame> frames_from(const page_batch& batch) {
    streams pair;
    require(page_stream::send(pair.writer, batch), "fixture send");
    std::vector<page_frame> out;
    for (size_t i = 0; i < batch.size().records; ++i) {
        page_frame frame;
        require(page_stream::read(pair.reader, frame), "fixture decode");
        out.push_back(std::move(frame));
    }
    return out;
}

static std::vector<page_frame> frames() {
    return frames_from(*fixture());
}

static shared_db memory(db_watchers* watchers = nullptr) {
    auto database = db::create_mdb(4);
    require(database->open("", watchers), "memory open");
    return database;
}

static std::string page(shared_db database, const std::string& owner,
    uint64_t id, bool present) {
    std::string out = "sentinel";
    require(database->transact_pages({owner}, [&](page_transaction& view) {
        require(view.lookup(owner, id, out) == (present ?
            db::lookup_status::found : db::lookup_status::missing),
            "replica page lookup");
        return true;
    }) == db::batch_status::ok, "replica page read");
    require(present || out == "sentinel", "missing page changed output");
    return out;
}

class events : public db_watcher {
public:
    size_t sets = 0;
    size_t deletions = 0;
    bool on_set(const std::string&, const std::string&, bool) override {
        ++sets;
        return true;
    }
    bool on_get(const std::string&, const std::string&, bool) override {
        return true;
    }
    bool on_del(const std::string&, bool) override {
        ++deletions;
        return true;
    }
};

static void test_stream_and_apply() {
    const auto batch = fixture();
    streams pair;
    require(page_stream::send(pair.writer, *batch), "send batch");
    std::string bytes(batch->size().bytes, '\0');
    require(pair.reader->read(&bytes[0], bytes.size(), true) ==
        static_cast<int>(bytes.size()), "read batch bytes");
    const unsigned char expected[] = {
        'P', 'K', 'V', '2', 0, 0, 0, 5, 0, 0, 0, 7, 0, 0, 0, 0,
        0, 0, 0, 16, 0, 0, 0, 2, 0, 0, 0, 3,
        0, 0, 0, 0, 0, 0, 0, 209
    };
    require(std::memcmp(bytes.data(), expected, sizeof(expected)) == 0,
        "exact independent BEGIN wire bytes");
    streams fragmented;
    std::thread writer([&] {
        for (size_t i = 0; i < bytes.size(); ++i) {
            require(fragmented.writer->write(bytes.data() + i, 1) == 1,
                "fragmented send");
        }
    });
    events observer;
    db_watchers watchers;
    watchers.add_watcher(&observer);
    auto destination = memory(&watchers);
    require(destination->set("", "old") &&
        destination->set(binary, "keep-root"), "destination seed");
    auto old_roots = roots();
    old_roots[0].action = action::keep;
    const std::vector<page_change> old = {{"", 2, "old-page", action::write}};
    require(destination->replica_apply_pages(old_roots, old) ==
        db::batch_status::ok, "initial page seed");
    observer.sets = observer.deletions = 0;
    page_receiver receiver;
    std::shared_ptr<const page_batch> received;
    for (size_t i = 0; i < batch->size().records; ++i) {
        page_frame frame;
        require(page_stream::read(fragmented.reader, frame), "fragmented read");
        const auto status = receiver.accept(std::move(frame), received);
        if (i + 1 != batch->size().records) {
            std::string root;
            require(status == result::pending && !received &&
                destination->get("", root) && root == "old" &&
                page(destination, "", 2, true) == "old-page",
                "partial batch changed destination");
        } else {
            require(status == result::complete && received &&
                received->apply(*destination) == db::batch_status::ok,
                "complete apply");
        }
    }
    writer.join();
    std::string root;
    require(destination->get("", root) && root == "root-a" &&
        destination->get(binary, root) && root == "keep-root" &&
        page(destination, "", 1, true) == "one" &&
        page(destination, binary, UINT64_MAX, true).empty(),
        "root/page apply mismatch");
    page(destination, "", 2, false);
    require(observer.sets == 0 && observer.deletions == 0,
        "replica transaction echoed observer events");
    require(received->apply(*destination) == db::batch_status::ok,
        "idempotent completed transaction failed");
}

static void expect_bad(std::vector<page_frame> input) {
    page_receiver receiver;
    std::shared_ptr<const page_batch> out;
    bool rejected = false;
    for (auto& frame : input) {
        const auto status = receiver.accept(std::move(frame), out);
        require(status != result::complete && !out,
            "invalid transaction completed");
        if (status == result::error) {
            rejected = true;
            break;
        }
    }
    require(rejected && !receiver.active(), "invalid transaction accepted");
    auto valid = frames();
    require(receiver.accept(std::move(valid[0]), out) == result::error,
        "poisoned receiver accepted new data");
    receiver.reset();
    require(receiver.accept(std::move(frames()[0]), out) == result::pending,
        "receiver did not reset");
}

static void test_invalid_sequences() {
    const auto original = frames();
    expect_bad({original[1]});
    expect_bad({original[0], original[0]});
    expect_bad({original[0], original.back()});
    expect_bad({original[0], original[3]});
    auto bad = original;
    bad[1].database = 8;
    expect_bad(std::move(bad));
    bad = original;
    bad[2].key = "";
    expect_bad(std::move(bad));
    bad = original;
    bad[2].key = "a";
    expect_bad(std::move(bad));
    bad = original;
    bad[3].key = "unknown";
    expect_bad(std::move(bad));
    bad = original;
    bad[4].id = 1;
    expect_bad(std::move(bad));
    bad = original;
    bad[3].id = 0;
    expect_bad(std::move(bad));
    bad = original;
    bad[1].operation = page_operation::root_erase;
    bad[1].value.clear();
    expect_bad(std::move(bad));
    bad = original;
    bad[1].value.assign(16385, 'x');
    expect_bad(std::move(bad));
    bad = original;
    bad[3].value.assign(16385, 'x');
    expect_bad(std::move(bad));
    bad = original;
    bad[3].key.assign(65526, 'x');
    expect_bad(std::move(bad));
    for (unsigned mode = 0; mode < 5; ++mode) {
        bad = original;
        auto& begin = bad[0].value;
        if (mode == 0) page_wire::put32(&begin[0], 0);
        if (mode == 1) page_wire::put32(&begin[4], 65536);
        if (mode == 2) page_wire::put64(&begin[8], page_batch::max_bytes + 1);
        if (mode == 3) page_wire::put64(&begin[8], 208);
        if (mode == 4) page_wire::put64(&begin[8], 210);
        expect_bad(std::move(bad));
    }
    bad = original;
    bad.back().id = 1;
    expect_bad(std::move(bad));
    bad = original;
    bad[0].database = 65536;
    expect_bad(std::move(bad));
}

static void test_truncation_and_reset() {
    const auto original = frames();
    for (size_t count = 1; count < original.size(); ++count) {
        page_receiver receiver;
        std::shared_ptr<const page_batch> out;
        for (size_t i = 0; i < count; ++i) {
            require(receiver.accept(original[i], out) == result::pending &&
                !out, "prefix completed without COMMIT");
        }
        receiver.reset();
        require(!receiver.active(), "disconnect retained staged pages");
        for (size_t i = 0; i < original.size(); ++i) {
            require(receiver.accept(original[i], out) ==
                (i + 1 == original.size() ? result::complete : result::pending),
                "reconnect failed to receive new transaction");
        }
    }
    auto batch = fixture();
    streams encoded;
    require(page_stream::send(encoded.writer, *batch), "truncation fixture");
    std::string bytes(batch->size().bytes, '\0');
    encoded.reader->read(&bytes[0], bytes.size(), true);
    for (size_t size = 0; size < bytes.size(); ++size) {
        streams pair;
        if (size) require(pair.writer->write(bytes.data(), size) ==
            static_cast<int>(size), "truncated write");
        pair.writer->close();
        page_receiver receiver;
        std::shared_ptr<const page_batch> out;
        page_frame frame;
        while (page_stream::read(pair.reader, frame)) {
            require(receiver.accept(std::move(frame), out) == result::pending,
                "truncated stream completed");
        }
        require(!out, "truncated stream returned a transaction");
    }
}

static void test_frame_validation() {
    auto original = frames()[3];
    for (unsigned mode = 0; mode < 8; ++mode) {
        char header[20] = {};
        page_wire::put32(header, page_wire::magic);
        page_wire::put32(header + 4, 9);
        page_wire::put32(header + 16, 8);
        if (mode == 0) header[3] = '1';
        if (mode == 1) page_wire::put32(header + 4, 99);
        if (mode == 2) page_wire::put32(header + 8, 65536);
        if (mode == 3) page_wire::put32(header + 12, 65526);
        if (mode == 4) page_wire::put32(header + 16, 7);
        if (mode == 5) page_wire::put32(header + 16, 16393);
        if (mode == 6) page_wire::put32(header + 4, 5);
        if (mode == 7) page_wire::put32(header + 4, 11);
        streams pair;
        pair.writer->write(header, sizeof(header));
        pair.writer->close();
        auto out = original;
        require(!page_stream::read(pair.reader, out) &&
            out.id == original.id && out.value == original.value,
            "invalid frame changed output");
    }
}

static void test_capture_bounds() {
    auto entries = roots();
    auto changes = pages();
    page_batch_size size;
    allocation_budget = 0;
    const bool measured = page_batch::measure(7, entries, changes, size);
    allocation_budget = -1;
    require(measured && size.bytes == 209, "measurement allocated memory");
    changes.push_back(changes.back());
    require(!page_batch::capture(7, entries, changes),
        "duplicate page accepted");
    changes = pages();
    changes[0].id = 0;
    require(!page_batch::capture(7, entries, changes), "zero id captured");
    changes = pages();
    changes[0].value.assign(16385, 'x');
    require(!page_batch::capture(7, entries, changes),
        "oversize page captured");
    require(!page_batch::capture(65536, entries, pages()),
        "invalid DB captured");
    entries.clear();
    entries.emplace_back("owner", true, "meta");
    entries.back().action = action::write;
    changes.clear();
    for (uint64_t id = 1; id <= page_batch::max_records - 3; ++id) {
        changes.emplace_back("owner", id, "", action::write);
    }
    require(page_batch::measure(0, entries, changes, size) &&
        size.records == page_batch::max_records, "exact record cap rejected");
    changes.emplace_back("owner", page_batch::max_records, "", action::write);
    require(!page_batch::measure(0, entries, changes, size),
        "record count exceeded cap");
}

static void test_delete_and_missing_base() {
    const auto batch = fixture();
    auto destination = memory();
    require(batch->apply(*destination) == db::batch_status::aborted,
        "ROOT_KEEP created an absent destination root");
    std::string value;
    require(!destination->get("", value), "failed base check changed root");
    page(destination, "", 1, false);
    require(destination->set(binary, "keep-root") &&
        batch->apply(*destination) == db::batch_status::ok,
        "delete setup");
    auto entries = roots();
    entries[0].action = action::erase;
    auto deletion = page_batch::capture(7, entries, {});
    require(deletion && deletion->size().records == 3, "root-only deletion");
    auto input = frames_from(*deletion);
    require(input[1].operation == page_operation::root_erase &&
        input[1].value.empty(), "root erase frame");
    page_receiver receiver;
    std::shared_ptr<const page_batch> out;
    for (size_t i = 0; i < input.size(); ++i) {
        const auto status = receiver.accept(std::move(input[i]), out);
        require(status == (i + 1 == input.size() ? result::complete :
            result::pending), "root deletion receive");
    }
    require(out->apply(*destination) == db::batch_status::ok &&
        !destination->get("", value) &&
        destination->get(binary, value) && value == "keep-root",
        "root-only deletion native apply");
    page(destination, "", 1, false);
    page(destination, binary, UINT64_MAX, true);
    auto unsupported = db::create_mdb_avl();
    require(unsupported->open("") && batch->apply(*unsupported) ==
        db::batch_status::unsupported, "unsupported engine accepted pages");
}

class page_recorder : public events {
public:
    std::shared_ptr<const page_batch> latest;
    bool supports_pages() const override { return true; }
    bool on_pages_database(unsigned database,
        const std::vector<db::batch_entry>& roots,
        const std::vector<page_change>& pages) override {
        latest = page_batch::capture(database, roots, pages);
        return latest != nullptr;
    }
};

static std::vector<std::string> stored_list(shared_db database,
    const std::string& key) {
    std::vector<std::string> values;
    require(database->transact_pages({key}, [&](page_transaction& view) {
        list_store list(view, key);
        return list.open() && list.range(0, -1, values);
    }) == db::batch_status::ok, "replicated List read");
    return values;
}

static void apply_page_frames(cluster_client& receiver,
    const std::shared_ptr<const page_batch>& batch) {
    require(batch != nullptr, "page event missing");
    auto input = frames_from(*batch);
    for (auto& frame : input) {
        rpc_packet packet;
        packet.page = true;
        packet.frame = std::move(frame);
        require(receiver.apply_packet(std::move(packet)), "page event route");
    }
}

static void test_edit_copy_move_replication() {
    page_recorder source_events, target_events;
    db_watchers source_watchers(0), target_watchers(7);
    source_watchers.add_watcher(&source_events);
    target_watchers.add_watcher(&target_events);
    auto source = memory(&source_watchers), target = memory(&target_watchers);
    auto zero = memory();
    auto pool = std::make_shared<database_pool>(zero, 16,
        [](size_t) { return memory(); });
    cluster_client receiver("", zero, pool);
    require(receiver.apply(std::make_shared<kv_message>("", "",
        message_oper_ready, 0, 2)), "edit receiver READY");
    require(source->transact_pages({"source"}, [](page_transaction& view) {
        list_store list(view, "source");
        list.initialize(4102444800123);
        return list.push({"before", std::string(1000, 'x'), "after"}, false) &&
            list.finish();
    }) == db::batch_status::ok, "replicated edit seed");
    apply_page_frames(receiver, source_events.latest);
    require(source->transact_pages({"source"}, [](page_transaction& view) {
        list_store list(view, "source");
        bool found;
        return list.open() && list.replace(1, "changed", found) && found &&
            list.finish();
    }) == db::batch_status::ok, "replicated middle replacement");
    apply_page_frames(receiver, source_events.latest);
    require(stored_list(zero, "source") == std::vector<std::string>({"before",
        "changed", "after"}), "replicated middle data");
    require(target->transact_pages({"copied"}, [](page_transaction& view) {
        list_store list(view, "copied");
        return list.open() && list.push({std::string(40000, 'o')}, false) &&
            list.finish();
    }) == db::batch_status::ok && target_events.latest &&
        target_events.latest->apply(*pool->get(7)) == db::batch_status::ok,
        "old target page seed");
    const auto previous_source_event = source_events.latest;
    require(source->copy_pages_to("source", *target, "copied",
        [](page_transaction& from, page_transaction& to) {
            list_store original(from, "source"), copy(to, "copied");
            bool copied;
            return original.open() && copy.open() &&
                original.clone_to(copy, copied) && copied && original.finish()
                    && copy.finish();
        }) == db::batch_status::ok && target_events.latest &&
        target_events.latest->database() == 7 &&
        source_events.latest == previous_source_event,
        "cross-DB copy published source or wrong destination DB");
    apply_page_frames(receiver, target_events.latest);
    require(stored_list(pool->get(7), "copied") == stored_list(zero, "source"),
        "cross-DB copy replica data");
    require(pool->get(7)->transact_pages({"copied"}, [](page_transaction& view)
        {
        std::string value;
        return view.lookup("copied", 2, value) == db::lookup_status::missing;
    }) == db::batch_status::ok, "replicated copy retained old target pages");
    require(source->transact_pages({"source", "moved"},
        [](page_transaction& view) {
            list_store original(view, "source"), target_list(view, "moved");
            std::string value;
            bool found;
            return original.open() && target_list.open() &&
                original.move_to(target_list, true, false, value, found) &&
                found && value == "before" && original.finish() &&
                target_list.finish();
        }) == db::batch_status::ok, "replicated move source");
    apply_page_frames(receiver, source_events.latest);
    require(stored_list(zero, "source") ==
        std::vector<std::string>({"changed", "after"}) &&
        stored_list(zero, "moved") == std::vector<std::string>{"before"},
        "atomic move replica metadata/pages");
}

static void test_list_replace_replication() {
    page_recorder observer;
    db_watchers watchers;
    watchers.add_watcher(&observer);
    auto source = memory(&watchers);
    auto destination = memory();
    require(source->transact_pages({"list"}, [](page_transaction& view) {
        list_store list(view, "list");
        return list.open() && list.push(std::vector<std::string>(30,
            std::string(1000, 'o')), false) && list.finish();
    }) == db::batch_status::ok && observer.latest &&
        observer.latest->apply(*destination) == db::batch_status::ok,
        "List source/replica seed");
    require(source->transact_pages({"list"}, [](page_transaction& view) {
        list_store list(view, "list");
        if (!list.open()) return false;
        list.initialize(4102444800123);
        std::string old = "sentinel";
        require(view.lookup("list", 2, old) == db::lookup_status::missing &&
            old == "sentinel", "reset exposed old physical pages");
        return list.push({"new"}, false) && list.finish();
    }) == db::batch_status::ok && observer.latest &&
        observer.latest->roots().front().clear_pages,
        "List replacement publication");
    auto input = frames_from(*observer.latest);
    require(input[1].operation == page_operation::root_replace,
        "List replacement lost wire reset marker");
    page_receiver receiver;
    std::shared_ptr<const page_batch> out;
    for (auto& frame : input) receiver.accept(std::move(frame), out);
    require(out && out->apply(*destination) == db::batch_status::ok &&
        destination->transact_pages({"list"}, [](page_transaction& view) {
            list_store list(view, "list");
            std::vector<std::string> values;
            std::string old;
            return list.open() && list.metadata().expires_at_ms ==
                4102444800123 && list.range(0, -1, values) && values ==
                    std::vector<std::string>{"new"} &&
                view.lookup("list", 2, old) == db::lookup_status::missing;
        }) == db::batch_status::ok, "replicated reset left stale List pages");
}

static void test_payload_failure() {
    char header[20] = {};
    page_wire::put32(header, page_wire::magic);
    page_wire::put32(header + 4, 7);
    page_wire::put32(header + 16, 1000);
    streams pair;
    const std::string payload(1000, 'x');
    require(pair.writer->write(payload.data(), payload.size()) == 1000,
        "payload failure fixture");
    page_frame out;
    out.key = "sentinel";
    allocation_budget = 0;
    const bool ok = page_stream::read_payload(pair.reader, header, out);
    allocation_budget = -1;
    require(!ok && out.key == "sentinel", "failed payload changed output");
    page_wire::put32(header + 4, 9);
    page_wire::put32(header + 16, 8);
    streams zero;
    const char number[8] = {};
    zero.writer->write(number, sizeof(number));
    require(!page_stream::read_payload(zero.reader, header, out) &&
        out.key == "sentinel", "zero wire page id accepted");
}

static void test_allocation_failures() {
    auto entries = roots();
    entries[0].value.assign(100, 'r');
    auto changes = pages();
    changes[0].value.assign(1000, 'p');
    bool succeeded = false;
    size_t failures = 0;
    for (long budget = 0; budget < 128; ++budget) {
        allocation_budget = budget;
        auto batch = page_batch::capture(7, entries, changes);
        allocation_budget = -1;
        if (batch) {
            succeeded = true;
            break;
        }
        ++failures;
    }
    require(succeeded && failures > 0, "capture allocation sweep incomplete");
    auto batch = page_batch::capture(7, entries, changes);
    const auto original = frames_from(*batch);
    succeeded = false;
    failures = 0;
    for (long budget = 0; budget < 128; ++budget) {
        auto input = original;
        page_receiver receiver;
        std::shared_ptr<const page_batch> out;
        allocation_budget = budget;
        result status = result::pending;
        for (auto& frame : input) {
            status = receiver.accept(std::move(frame), out);
            if (status == result::error) break;
        }
        allocation_budget = -1;
        if (status == result::complete) {
            require(out && out->size().bytes == batch->size().bytes,
                "receiver allocation success mismatch");
            succeeded = true;
            break;
        }
        require(status == result::error && !out && !receiver.active(),
            "failed receiver retained a partial batch");
        ++failures;
    }
    require(succeeded && failures > 0, "receiver allocation sweep incomplete");
    succeeded = false;
    failures = 0;
    for (long budget = 0; budget < 256; ++budget) {
        auto destination = memory();
        require(destination->set("", "old") &&
            destination->set(binary, "keep-root"), "apply failure setup");
        allocation_budget = budget;
        const auto status = batch->apply(*destination);
        allocation_budget = -1;
        if (status == db::batch_status::ok) {
            require(page(destination, "", 1, true) == std::string(1000, 'p'),
                "successful fault-injected apply omitted page");
            succeeded = true;
            break;
        }
        require(status == db::batch_status::error, "apply failure status");
        std::string root;
        require(destination->get("", root) && root == "old" &&
            destination->get(binary, root) && root == "keep-root",
            "failed apply changed a root");
        page(destination, "", 1, false);
        page(destination, binary, UINT64_MAX, false);
        ++failures;
    }
    require(succeeded && failures > 0, "apply allocation sweep incomplete");
}

static void test_large_stream() {
    list_root root;
    root.elements = 1;
    root.head = 1;
    root.tail = 80;
    root.expires_at_ms = 4102444800123;
    std::string encoded;
    require(list_page_codec::encode_root(root, encoded) == list_page_status::ok,
        "large List root encode");
    std::vector<db::batch_entry> entries;
    entries.emplace_back("large", false, encoded);
    entries.back().action = action::write;
    std::vector<page_change> changes;
    const auto payload = list_page_codec::max_fragment_bytes;
    for (uint64_t id = 1; id <= root.tail; ++id) {
        list_page value;
        value.id = id;
        value.previous = id == 1 ? 0 : id - 1;
        value.next = id == root.tail ? 0 : id + 1;
        list_fragment fragment;
        fragment.total_bytes = root.tail * payload;
        fragment.offset = (id - 1) * payload;
        fragment.bytes.assign(payload, static_cast<char>(id));
        value.fragments.push_back(std::move(fragment));
        require(list_page_codec::encode_page(value, encoded) ==
            list_page_status::ok, "large List page encode");
        changes.emplace_back("large", id, encoded, action::write);
    }
    auto batch = page_batch::capture(15, entries, changes);
    require(batch && batch->size().bytes > 1024000,
        "large transaction fixture");
    streams pair;
    std::thread writer([&] {
        require(page_stream::send(pair.writer, *batch), "large stream send");
    });
    page_receiver receiver;
    std::shared_ptr<const page_batch> out;
    for (size_t i = 0; i < batch->size().records; ++i) {
        page_frame frame;
        require(page_stream::read(pair.reader, frame) &&
            frame.value.size() <= 16384,
            "large transaction used an oversized frame");
        require(receiver.accept(std::move(frame), out) ==
            (i + 1 == batch->size().records ?
                result::complete : result::pending), "large stream receive");
    }
    writer.join();
    auto zero = memory();
    require(zero->set("large", "db-zero"), "DB zero isolation seed");
    database_pool pool(zero, 16, [](size_t) { return memory(); });
    auto destination = out ? pool.get(out->database()) : nullptr;
    require(out && out->database() == 15 && destination &&
        out->apply(*destination) == db::batch_status::ok,
        "large stream native apply");
    require(destination->get("large", encoded) && encoded.size() == 40 &&
        list_page_codec::decode_root(encoded, root) == list_page_status::ok &&
        root.elements == 1 && root.tail == 80 &&
        root.expires_at_ms == 4102444800123, "replicated List metadata");
    list_page last;
    require(list_page_codec::decode_page(page(destination, "large", 80, true),
        last) == list_page_status::ok && last.next == 0 &&
        last.fragments.front().offset == 79 * payload &&
        last.fragments.front().bytes == std::string(payload, char(80)),
        "replicated List fragment");
    require(zero->get("large", encoded) && encoded == "db-zero",
        "DB N transaction changed DB zero");
}

static void test_maintenance_wire() {
    auto entries = roots();
    entries.front().clear_pages = true;
    entries.front().maintenance_only = true;
    auto batch = page_batch::capture(0, entries, pages());
    require(batch != nullptr, "maintenance capture");
    auto encoded = frames_from(*batch);
    require(encoded[1].operation == page_operation::root_maintenance,
        "maintenance opcode missing");
    page_receiver receiver;
    std::shared_ptr<const page_batch> out;
    for (size_t i = 0; i < encoded.size(); ++i)
        require(receiver.accept(std::move(encoded[i]), out) ==
            (i + 1 == encoded.size() ? result::complete : result::pending),
            "maintenance frame rejected");
    require(out && out->roots().front().clear_pages &&
        out->roots().front().maintenance_only,
        "maintenance marker lost in receiver");
    entries.front().clear_pages = false;
    require(!page_batch::capture(0, entries, pages()),
        "maintenance without reset accepted");
}

static void test_small_batch_preflight() {
    struct page_observer : events {
        bool supports_pages() const override { return true; }
    } observer;
    for (unsigned index : {0U, page_batch::max_databases}) {
        db_watchers watchers(index);
        watchers.add_watcher(&observer);
        for (auto kind : {action::keep, action::write, action::erase})
        for (unsigned flags = 0; flags < 16; ++flags)
        for (unsigned shape = 0; shape < 8; ++shape) {
            std::vector<db::batch_entry> entries;
            entries.emplace_back("root", (flags & 1) != 0, "value");
            auto& root = entries.front();
            root.action = kind;
            root.clear_pages = (flags & 2) != 0;
            root.maintenance_only = (flags & 4) != 0;
            root.force_watch_invalidation = (flags & 8) != 0;
            std::vector<page_change> changes;
            if (shape) changes.emplace_back(shape == 1 ? "other" : "root",
                shape == 2 ? 0 : 1, "value", action::write);
            if (shape >= 4) changes.emplace_back("root", shape == 4 ? 1 : 2,
                shape == 5 ? "invalid" : "", shape == 6 ? action::keep :
                    action::erase);
            page_batch_size measured;
            require(watchers.can_pages(entries, changes) ==
                page_batch::measure(index, entries, changes, measured),
                "small preflight changed accepted batches");
        }
    }
}

int main() {
    acl::acl_cpp_init();
    test_stream_and_apply();
    test_maintenance_wire();
    test_invalid_sequences();
    test_truncation_and_reset();
    test_frame_validation();
    test_capture_bounds();
    test_small_batch_preflight();
    test_delete_and_missing_base();
    test_edit_copy_move_replication();
    test_list_replace_replication();
    test_payload_failure();
    test_allocation_failures();
    test_large_stream();
    std::cout << "Bounded page transaction stream tests passed\n";
}
