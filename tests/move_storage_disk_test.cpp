#include "stdafx.h"
#include "move_recovery_types.h"
#include "db/move_storage.h"
#include "coder/record_codec.h"
#include <sys/resource.h>
#include <unistd.h>

static const int64_t deadline = 4102444800000LL;
static const char* storage_points[] = {
    "validated", "source-synced", "decided", "completed"};
static const char* apply_points[] = {"source-synced", "target-cleared",
    "page", "target-published", "target-synced", "source-erased",
    "source-erased-synced", "completed"};
static unsigned sync_calls(shared_db database) {
#ifdef HAS_ROCKSDB
    if (auto* value = dynamic_cast<recovery_probe<rdb>*>(database.get()))
        return value->syncs;
#endif
#ifdef HAS_WT
    if (auto* value = dynamic_cast<recovery_probe<wdb>*>(database.get()))
        return value->syncs;
#endif
    require(false, "native coordinator sync counter"); return 0;
}
struct storage_io : recovery_io {
    int parent = -1;
    int directory(const char* path) override {
        parent = move_journal_io::directory(path); return parent;
    }
    long long write(int fd, const char* bytes, size_t size) override {
        if (armed && fault == "decision-write") return failure();
        return move_journal_io::write(fd, bytes, size);
    }
    int sync(int fd) override {
        if (armed && (fault == "decision-sync" ||
            (fault == "decision-dir-sync" && fd == parent))) return failure();
        return recovery_io::sync(fd);
    }
    int replace(const char* from, const char* to) override {
        if (armed && fault == "decision-rename") return failure();
        return move_journal_io::replace(from, to);
    }
    int failure() {
        std::cerr << "FAULT " << fault << '\n'; errno = EIO; return -1;
    }
};
static move_storage::callbacks policy(shared_db source,
    const std::function<void()>& complete) {
    move_storage::callbacks out;
    out.prepare = [source](const move_storage::roots& roots) {
        record_header header;
        if (!roots.source_found) return move_storage::choice::skip;
        const auto& codec = source->value_codec();
        if (codec.stored_header(roots.source, header) != codec_status::ok)
            return move_storage::choice::error;
        const auto now = unix_time_ms();
        if (header.expires_at_ms >= 0 && header.expires_at_ms <= now)
            return move_storage::choice::skip;
        if (roots.target_found) {
            if (codec.stored_header(roots.target, header) != codec_status::ok)
                return move_storage::choice::error;
            if (header.expires_at_ms < 0 || header.expires_at_ms > now)
                return move_storage::choice::skip;
        }
        return move_storage::choice::proceed;
    };
    out.complete = complete; return out;
}
static void exercise(const std::string& backend, const std::string& root) {
    auto from = open_probe(backend, root, true, "");
    auto to = open_probe(backend, root, false, "");
    database_pool pool(from, 2, [to](size_t id) {
        return id == 1 ? to : shared_db();
    });
    move_journal journal;
    require(journal.open(root + "/decision", backend + root) ==
        move_journal::status::empty, "storage exercise journal");
    uint64_t completed = 0;
    auto call = policy(from, [&] {
        require(journal.pending(), "effects before decision"); ++completed;
    });
    auto transfer = [&](const std::string& key) {
        const auto before = snapshot(from, key);
        const auto serial = journal.last_serial();
        const auto source_syncs = sync_calls(from);
        const auto target_syncs = sync_calls(to);
        reset_metrics(to); bool moved = false;
        require(move_storage::run(pool, journal, 0, 1, key, call, moved) ==
            db::batch_status::ok && moved && journal.last_serial() ==
            serial + 1 && completed == serial + 1,
            "native coordinated transfer");
        require(sync_calls(from) == source_syncs + 2 &&
            sync_calls(to) == target_syncs + 1, "duplicate/missing WAL fence");
        std::string bytes;
        require(!from->get(key, bytes), "coordinator retained source");
        const auto after = snapshot(to, key);
        const auto measured = metrics(to);
        require(before.root == after.root && before.pages == after.pages &&
            before.crc == after.crc && measured.maximum_batch <= 1 &&
            measured.maximum_value <= 16384, "coordinator staging/bytes");
    };
    for (const auto& key : {std::string(), moved_key(), std::string("inline"),
        std::string("large"), std::string("expired-target")}) {
        seed(from, key, key == "inline" ? 0 : key == "large" ? 512 : 4,
            deadline);
        if (key == "expired-target") seed(to, key, 2, 1);
        require(raw_page(to, key, 9999, "orphan"), "coordinator orphan seed");
        transfer(key);
    }
    seed(from, "expired", 2, 1);
    seed(from, "blocked", 4, deadline); seed(to, "blocked", 2, deadline);
    for (const auto& key : {"absent", "expired", "blocked"}) {
        const auto serial = journal.last_serial(); bool moved = true;
        require(move_storage::run(pool, journal, 0, 1, key, call, moved) ==
            db::batch_status::ok && !moved && journal.last_serial() == serial,
            "coordinator policy skip changed decision");
    }
    check(from, "expired", true, 2, 1);
    check(from, "blocked", true, 4, deadline);
    check(to, "blocked", true, 2, deadline);
    typed_recovery(from, to, transfer);
    index(from, {"expired", "blocked"});
    index(to, {"", moved_key(), "inline", "large", "expired-target",
        "blocked", "hash", "list", "set", "zset"});
    std::cout << "Native MOVE coordinator bounded families and policy passed\n";
}
static void checkpoint(const std::string& requested, const std::string& point) {
    if (requested == "kill:" + point) {
        std::cout << "READY " << point << std::endl;
        for (;;) ::pause();
    }
    if (requested == "throw:" + point) {
        std::cerr << "FAULT " << requested << '\n'; throw std::bad_alloc();
    }
}
static void coordinate(shared_db from, shared_db to, database_pool& pool,
    move_journal& journal, storage_io& io, const std::string& fault) {
    auto call = policy(from, [&] { checkpoint(fault, "effects"); });
    if (fault == "throw:prepare")
        call.prepare = [&](const move_storage::roots&) -> move_storage::choice {
            checkpoint(fault, "prepare"); return move_storage::choice::error;
        };
    call.observe = [&](move_storage::step at) {
        if (at == move_storage::step::source_synced ||
            at == move_storage::step::decided)
            require(sync_calls(from) == 1 && !sync_calls(to) &&
                (journal.state() == move_journal::status::empty) ==
                (at == move_storage::step::source_synced),
                "native decision/WAL ordering");
        if ((at == move_storage::step::validated &&
                fault.compare(0, 9, "decision-") == 0) ||
            at == move_storage::step::decided) io.armed = true;
        checkpoint(fault, std::string("storage-") +
            storage_points[static_cast<unsigned>(at)]);
    };
    call.apply = [&](move_recovery::step at) {
        checkpoint(fault, std::string("apply-") +
            apply_points[static_cast<unsigned>(at)]);
    };
    bool moved = true;
    const auto result = move_storage::run(pool, journal, 0, 1, moved_key(),
        call, moved);
    if (fault == "source-read" || fault == "throw:prepare") {
        require(result == db::batch_status::error && !moved &&
            journal.state() == move_journal::status::empty,
            "predecision failure created a decision");
    } else require(result == db::batch_status::ok && moved,
        "coordinator did not commit");
}
int main(int argc, char** argv) {
    const struct rlimit no_core = {0, 0};
    ::setrlimit(RLIMIT_CORE, &no_core);
    require(argc == 5, "backend mode directory argument");
    acl::acl_cpp_init();
    const std::string backend(argv[1]), mode(argv[2]), root(argv[3]);
    const std::string argument(argv[4]);
    if (mode == "--exercise") { exercise(backend, root); return 0; }
    auto from = open_probe(backend, root, true,
        mode == "--run" ? argument : "");
    auto to = open_probe(backend, root, false,
        mode == "--run" ? argument : "");
    database_pool pool(from, 2, [to](size_t id) {
        return id == 1 ? to : shared_db();
    });
    storage_io io; io.fault = argument;
    move_journal journal(&io);
    require(journal.open(root + "/decision", backend + root) !=
        move_journal::status::failed, "coordinator journal open");
    if (mode == "--seed") {
        seed(from, moved_key(), 4, deadline);
        seed(from, "neighbor", 2, deadline); seed(to, "neighbor", 2, deadline);
        require(from->sync_key(moved_key()) == db::sync_status::ok &&
            from->sync_key("neighbor") == db::sync_status::ok &&
            to->sync_key("neighbor") == db::sync_status::ok,
            "coordinator fixture persistence");
    } else if (mode == "--run") {
        coordinate(from, to, pool, journal, io, argument);
    } else if (mode == "--recover") {
        move_recovery::run(pool, journal);
    } else {
        require(mode == "--check", "coordinator helper mode");
        const bool moved = argument == "moved";
        check(from, moved_key(), !moved, 4, deadline);
        check(to, moved_key(), moved, 4, deadline);
        check(from, "neighbor", true, 2, deadline);
        check(to, "neighbor", true, 2, deadline);
        require(journal.state() == move_journal::status::empty,
            "coordinator check pending journal");
    }
}
