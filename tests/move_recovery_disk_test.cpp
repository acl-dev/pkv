#include "stdafx.h"
#include "move_recovery_support.h"
#include "move_recovery_types.h"
#include <sys/resource.h>
#include <unistd.h>

static const int64_t deadline = 4102444800000LL;
static const char* points[] = {"source-synced", "target-cleared", "page",
    "target-published", "target-synced", "source-erased",
    "source-erased-synced",
    "completed"};
static void recover(shared_db source, shared_db target, move_journal& journal,
    const std::string& point) {
    if (point == "alias") target = source;
    if (point == "memory") {
        target = db::create_mdb(4); require(target->open(""), "memory open");
    }
    if (point == "alias" || point == "memory" || point == "unopened")
        std::cerr << "FAULT " << point << '\n';
    database_pool pool(source, 2, [target](size_t id) {
        return id == 1 ? target : shared_db();
    });
    reset_metrics(target);
    bool stopped = false;
    move_recovery::run(pool, journal, [&](move_recovery::step step) {
        const std::string name(points[static_cast<unsigned>(step)]);
        if (point == "throw:" + name) {
            std::cerr << "FAULT " << point << '\n'; throw std::bad_alloc();
        }
        if (!stopped && point == "kill:" + name) {
            stopped = true;
            std::cout << "READY " << name << std::endl;
            for (;;) ::pause();
        }
    });
    const auto measured = metrics(target);
    require(measured.maximum_batch <= 1 && measured.maximum_value <= 16384,
        "recovery collected the whole value");
    require(journal.state() == move_journal::status::empty,
        "recovery returned without clearing decision");
}
static void exercise(const std::string& backend, const std::string& root) {
    auto from = open_probe(backend, root, true, "");
    auto to = open_probe(backend, root, false, "");
    move_journal journal;
    require(journal.open(root + "/decision", backend + root) ==
        move_journal::status::empty, "exercise journal");
    std::vector<std::string> keys{"", moved_key(), "prefix", "prefixx"};
    for (unsigned bucket = 0; bucket < 2; ++bucket) {
        std::string key = "partition:" + std::to_string(bucket);
        while (acl_hash_crc32(key.data(), key.size()) % 2 != bucket) key += 'x';
        keys.push_back(key);
    }
    keys.push_back("inline");
    uint64_t serial = 0;
    for (const auto& key : keys) {
        std::string absent("unchanged");
        require(bounded(to, key, absent) == db::lookup_status::missing &&
            absent == "unchanged", "bounded missing root modified output");
        const int64_t expiry = ++serial % 2 ? deadline : 1;
        const size_t count = key == "inline" ? 0 : serial == 4 ? 512 : 4;
        seed(from, key, count, expiry);
        require(raw_page(to, key, 9999, "leftover"), "orphan seed");
        require(from->sync_key(key) == db::sync_status::ok,
            "exercise source fence");
        move_decision decision;
        decision.source = 0; decision.target = 1;
        decision.serial = serial; decision.owner = key;
        require(journal.decide(decision), "exercise decision");
        recover(from, to, journal, "");
        check(from, key, false, count, expiry);
        check(to, key, true, count, expiry);
    }
    require(visit(to, "prefix", [](uint64_t, const std::string&) {
        return false;
    }) == db::batch_status::aborted, "stream early stop");
    check(to, "prefix", true, 4, deadline);
    index(from, {});
    index(to, std::set<std::string>(keys.begin(), keys.end()));
    typed_recovery(from, to, [&](const std::string& key) {
        const auto before = snapshot(from, key);
        require(from->sync_key(key) == db::sync_status::ok,
            "typed source fence");
        move_decision decision;
        decision.source = 0; decision.target = 1;
        decision.serial = ++serial; decision.owner = key;
        require(journal.decide(decision), "typed durable decision");
        recover(from, to, journal, "");
        std::string missing;
        require(!from->get(key, missing), "typed source retained");
        const auto after = snapshot(to, key);
        require(before.root == after.root && before.pages == after.pages &&
            before.crc == after.crc, "typed native family bytes changed");
    });
    index(from, {});
    recover(from, to, journal, "");
    std::cout << "Bounded native redo, binary/expired keys "
        "and indexes passed\n";
}
int main(int argc, char** argv) {
    const struct rlimit no_core = {0, 0};
    ::setrlimit(RLIMIT_CORE, &no_core);
    require(argc == 5, "backend mode root argument");
    acl::acl_cpp_init();
    const std::string backend(argv[1]), mode(argv[2]), root(argv[3]);
    const std::string argument(argv[4]);
    if (mode == "--exercise") { exercise(backend, root); return 0; }
    const auto fault = mode == "--recover" ? argument : std::string();
    auto from = open_probe(backend, root, true, fault);
    auto to = open_probe(backend, root, false, fault);
    recovery_io io; io.fault = fault;
    move_journal journal(&io);
    const auto state = journal.open(root + "/decision", backend + root);
    if (mode == "--seed") {
        require(state == move_journal::status::empty, "seed journal state");
        if (argument != "both-missing") seed(from, moved_key(), 4, deadline);
        seed(from, "neighbor", 2, deadline); seed(to, "neighbor", 2, deadline);
        require(raw_page(to, moved_key(), 9999, "leftover"), "partial target");
        if (argument == "big-root") {
            require(from->set(moved_key(), std::string(16385, 'x')),
                "big root seed");
            std::string unchanged("unchanged");
            require(bounded(from, moved_key(), unchanged) ==
                db::lookup_status::error && unchanged == "unchanged",
                "bounded large root modified output");
        }
        if (argument == "big-page") require(raw_page(from, moved_key(), 2,
            std::string(16385, 'x')), "big page seed");
        if (argument == "zero-id") require(raw_page(from, moved_key(), 0,
            "bad"), "zero ID seed");
        require(from->sync_key(moved_key()) == db::sync_status::ok,
            "source WAL before decision");
        move_decision decision;
        decision.source = 0;
        decision.target = argument == "range" ? 2 : 1; decision.serial = 77;
        decision.owner = moved_key();
        require(journal.decide(decision), "seed durable decision");
    } else if (mode == "--recover") {
        io.armed = true; recover(from, to, journal, fault);
    } else {
        require(mode == "--check" && state == move_journal::status::empty,
            "check journal state");
        check(from, moved_key(), false, 4, deadline);
        check(to, moved_key(), true, 4, deadline);
        check(from, "neighbor", true, 2, deadline);
        check(to, "neighbor", true, 2, deadline);
        index(from, {"neighbor"}); index(to, {"neighbor", moved_key()});
    }
    std::cout << "Native MOVE recovery helper passed\n";
}
