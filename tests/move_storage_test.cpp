#include "stdafx.h"
#include "db/move_storage.h"
#include "db/database_pool.h"
#include "db/mdb/mdb.h"
#include "move_journal_test_support.h"
#include <chrono>
#include <future>
#include <limits>
#include <thread>

struct storage_probe : mdb {
    unsigned syncs = 0;
    bool capable = true, read_error = false;
    sync_status sync_result = sync_status::ok;
    storage_probe() : mdb(4) {}
    bool raw(const std::string& key, std::string& value) {
        return mdb::dbget(key, value);
    }
protected:
    bool dbsupports_sync() const override { return capable; }
    bool dbsupports_page_stream() const override { return capable; }
    lookup_status dblookup_bounded_root(const std::string& key,
        std::string& out) override {
        if (read_error) return lookup_status::error;
        std::string bytes;
        if (!mdb::dbget(key, bytes)) return lookup_status::missing;
        if (bytes.size() > 16384) return lookup_status::error;
        out.swap(bytes); return lookup_status::found;
    }
    batch_status dbvisit_pages(const std::string&,
        const page_visitor&) override { return batch_status::ok; }
    sync_status dbsync_key(const std::string&) override {
        ++syncs; return sync_result;
    }
};
struct fixture {
    journal_directory directory;
    move_journal journal;
    std::shared_ptr<storage_probe> from{new storage_probe};
    std::shared_ptr<storage_probe> to{new storage_probe};
    database_pool pool;
    fixture() : pool(from, 2, [this](size_t id) {
        return id == 1 ? to : shared_db();
    }) {
        require(from->open("") && to->open(""), "storage probe open");
        require(journal.open(directory.file(), "test physical pool") ==
            move_journal::status::empty, "storage probe journal");
        require(from->set("owner", "root"), "storage probe seed");
    }
    void unchanged() {
        std::string bytes;
        require(from->get("owner", bytes) && bytes == "root" &&
            !to->get("owner", bytes) && journal.state() ==
            move_journal::status::empty && journal.last_serial() == 0 &&
            ::access(directory.file().c_str(), F_OK),
            "predecision failure changed storage");
    }
};
static move_storage::callbacks actions(unsigned& completed) {
    move_storage::callbacks out;
    out.prepare = [](const move_storage::roots& view) {
        return view.source_found && !view.target_found ?
            move_storage::choice::proceed : move_storage::choice::skip;
    };
    out.complete = [&completed] { ++completed; };
    return out;
}
static void predecision() {
    fixture files;
    unsigned completed = 0, prepared = 0;
    auto call = actions(completed);
    bool moved = true;
    call.prepare = [&](const move_storage::roots&) {
        ++prepared; return move_storage::choice::skip;
    };
    require(move_storage::run(files.pool, files.journal, 0, 1, "owner",
        call, moved) == db::batch_status::ok && !moved && prepared == 1,
        "skipped preparation");
    call.prepare = [](const move_storage::roots&) -> move_storage::choice {
        throw std::bad_alloc();
    };
    require(move_storage::run(files.pool, files.journal, 0, 1, "owner",
        call, moved) == db::batch_status::error && !moved,
        "predecision exception");
    call = actions(completed); files.from->sync_result = db::sync_status::error;
    require(move_storage::run(files.pool, files.journal, 0, 1, "owner",
        call, moved) == db::batch_status::error && !moved,
        "failed source WAL accepted");
    files.from->sync_result = db::sync_status::ok;
    files.from->read_error = true;
    require(move_storage::run(files.pool, files.journal, 0, 1, "owner",
        call, moved) == db::batch_status::error && !moved,
        "failed root lookup accepted");
    files.from->read_error = false; files.to->capable = false;
    require(move_storage::run(files.pool, files.journal, 0, 1, "owner",
        call, moved) == db::batch_status::unsupported && !moved,
        "unsupported target accepted");
    files.to->capable = true;
    for (const auto target : {size_t(0), size_t(2)})
        require(move_storage::run(files.pool, files.journal, 0, target,
            "owner", call, moved) == db::batch_status::error && !moved,
            "invalid target accepted");
    call.complete = {};
    require(move_storage::run(files.pool, files.journal, 0, 1, "owner",
        call, moved) == db::batch_status::error && !moved,
        "missing effects callback accepted");
    files.unchanged(); require(!completed, "early completion callback");
}
static void serials() {
    fixture files;
    move_decision value;
    value.target = 1; value.owner = "owner";
    require(!files.journal.can_decide(value), "zero serial accepted");
    value.serial = 1;
    require(files.journal.can_decide(value) &&
        files.journal.state() == move_journal::status::empty &&
        !files.journal.last_serial(), "preflight changed journal");
    value.serial = std::numeric_limits<uint64_t>::max();
    require(files.journal.decide(value) && files.journal.complete(value.serial),
        "serial overflow seed");
    unsigned completed = 0;
    auto call = actions(completed);
    bool moved = true;
    require(move_storage::run(files.pool, files.journal, 0, 1, "owner",
        call, moved) == db::batch_status::error && !moved && !completed,
        "serial overflow accepted");
}
static void isolation() {
    fixture files;
    unsigned completed = 0;
    auto call = actions(completed);
    std::vector<std::future<bool>> writers;
    call.observe = [&](move_storage::step point) {
        if (point == move_storage::step::validated) {
            for (auto database : {files.from, files.to}) {
                auto entered = std::make_shared<std::promise<void>>();
                auto started = entered->get_future();
                writers.push_back(std::async(std::launch::async,
                    [database, entered] {
                    entered->set_value();
                    return database->set("owner", "after-unlock");
                }));
                started.get();
            }
        }
        for (auto& writer : writers)
            require(writer.wait_for(std::chrono::milliseconds(30)) ==
                std::future_status::timeout, "writer entered retained locks");
        if (point == move_storage::step::decided)
            require(files.journal.pending() && files.from->syncs == 1 &&
                !files.to->syncs, "decision preceded source WAL");
    };
    call.complete = [&] {
        std::string bytes;
        require(!files.from->raw("owner", bytes) &&
            files.to->raw("owner", bytes) && bytes == "root" &&
            files.from->syncs == 2 && files.to->syncs == 1 &&
            files.journal.pending(), "effects phase ordering");
        ++completed;
    };
    bool moved = false;
    require(move_storage::run(files.pool, files.journal, 0, 1, "owner",
        call, moved) == db::batch_status::ok && moved && completed == 1 &&
        files.journal.state() == move_journal::status::empty &&
        files.journal.last_serial() == 1, "retained storage commit");
    for (auto& writer : writers) require(writer.get(), "blocked writer failed");
    std::string bytes;
    require(files.from->get("owner", bytes) && bytes == "after-unlock" &&
        files.to->get("owner", bytes) && bytes == "after-unlock",
        "writers failed after lock release");
}
static void stable_key() {
    fixture files;
    unsigned completed = 0;
    auto call = actions(completed);
    std::string owner("owner");
    call.prepare = [&](const move_storage::roots& view) {
        require(view.owner == "owner" && view.source_found &&
            view.source == "root" &&
            !view.target_found, "stable owner snapshot");
        owner = "other";
        return move_storage::choice::proceed;
    };
    bool moved = false;
    require(move_storage::run(files.pool, files.journal, 0, 1, owner,
        call, moved) == db::batch_status::ok && moved && completed == 1,
        "stable owner operation");
    std::string bytes;
    require(!files.from->get("owner", bytes) &&
        files.to->get("owner", bytes) && bytes == "root" &&
        !files.to->get("other", bytes),
        "caller alias redirected the committed owner");
    std::cout << "Stable owned MOVE key passed\n";
}
int main() {
    acl::acl_cpp_init();
    predecision(); serials(); isolation(); stable_key();
    std::cout << "Retained MOVE preparation, WAL ordering "
        "and isolation passed\n";
}
