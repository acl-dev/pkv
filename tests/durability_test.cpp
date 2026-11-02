#include "stdafx.h"
#include "db/mdb/mdb.h"
#include "db/page_workspace.h"
#include "db/db_watcher.h"
#include "coder/string_page.h"
#include "coder/record_codec.h"
#include <chrono>
#include <condition_variable>
#include <future>
#include <iostream>
#include <thread>

using namespace pkv;
static void require(bool ok, const char* reason) {
    if (!ok) { std::cerr << reason << '\n'; std::exit(1); }
}
struct probe : mdb {
    probe() : mdb(4) {}
    sync_status result = sync_status::ok;
    bool fail = false, capable = true;
    unsigned calls = 0;
    std::function<void()> hook;
protected:
    bool dbsupports_sync() const override { return capable; }
    sync_status dbsync_key(const std::string&) override {
        ++calls;
        if (hook) hook();
        if (fail) throw std::bad_alloc();
        return result;
    }
};
struct failed_probe : probe {
protected:
    bool dbopen(const char*) override { return false; }
};
struct observer : db_watcher {
    unsigned writes = 0;
    bool supports_pages() const override { return true; }
    bool on_pages_database(unsigned, const std::vector<db::batch_entry>&,
        const std::vector<page_change>&) override { ++writes; return true; }
    bool on_set(const std::string&, const std::string&, bool) override {
        ++writes; return true;
    }
    bool on_del(const std::string&, bool) override { ++writes; return true; }
    bool on_get(const std::string&, const std::string&, bool) override {
        return true;
    }
};
static std::string root() {
    string_root value;
    value.length = 1; value.inline_value = "x";
    value.expires_at_ms = unix_time_ms() + 3600000;
    std::string out;
    require(string_page_codec::encode_root(value, out) ==
        string_page_status::ok, "sync root");
    return out;
}
static void outcomes() {
    observer events;
    db_watchers watchers;
    watchers.add_watcher(&events);
    auto database = std::make_shared<probe>();
    require(database->open("", &watchers), "sync probe open");
    const std::string key("key\0binary", 10), original = root();
    require(database->set(key, original), "sync seed");
    std::vector<watch_token> watched;
    require(database->watch({key}, watched) == db::batch_status::ok,
        "sync WATCH");
    auto waiting = database->wait_for_keys({key});
    events.writes = 0;
    for (auto result : {db::sync_status::ok, db::sync_status::unsupported,
        db::sync_status::error}) {
        database->result = result;
        require(database->sync_key(key) == result, "sync outcome propagation");
    }
    database->fail = true;
    require(database->sync_key(key) == db::sync_status::error,
        "sync allocation exception");
    database->fail = false;
    database->capable = false;
    require(database->sync_key(key) == db::sync_status::unsupported,
        "unsupported barrier entered engine");
    std::string after;
    key_wait_event event;
    require(database->calls == 4 && !events.writes &&
        database->get(key, after) && after == original &&
        watched[0]->valid(unix_time_ms()) && !waiting->wait(0, event),
        "sync changed value/TTL/WATCH/observer/readiness");
}
static void unsupported() {
    auto failed = std::make_shared<failed_probe>();
    require(!failed->open("") && failed->sync_key("") ==
        db::sync_status::error && !failed->calls,
        "failed initialization reached WAL engine");
    auto memory = db::create_mdb(4);
    require(memory->open("") && memory->sync_key("") ==
        db::sync_status::unsupported, "volatile MDB advertised durability");
#ifdef HAS_TBB
    auto tbb = db::create_mdb_tbb(4);
    require(tbb->open("") && tbb->sync_key("") == db::sync_status::unsupported,
        "volatile oneTBB advertised durability");
#endif
    auto workspace = std::make_shared<page_workspace>(memory);
    require(workspace->sync_key("") == db::sync_status::unsupported,
        "workspace advertised durability");
#ifdef HAS_ROCKSDB
    require(db::create_rdb()->sync_key("") == db::sync_status::error,
        "unopened RocksDB barrier");
#endif
#ifdef HAS_WT
    require(db::create_wdb()->sync_key("") == db::sync_status::error,
        "unopened WiredTiger barrier");
#endif
}
static void ordering() {
    auto database = std::make_shared<probe>();
    require(database->open("") && database->set("owner", "old"),
        "sync ordering seed");
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false, released = false;
    database->hook = [&] {
        std::unique_lock<std::mutex> guard(mutex);
        entered = true; changed.notify_all();
        changed.wait(guard, [&] { return released; });
    };
    std::promise<db::sync_status> done;
    auto fenced = done.get_future();
    std::thread fence([&] { done.set_value(database->sync_key("owner")); });
    {
        std::unique_lock<std::mutex> guard(mutex);
        require(changed.wait_for(guard, std::chrono::seconds(3),
            [&] { return entered; }), "sync hook did not enter");
    }
    std::promise<bool> wrote;
    auto written = wrote.get_future();
    std::promise<void> attempting;
    auto started = attempting.get_future();
    std::thread writer([&] {
        attempting.set_value();
        wrote.set_value(database->set("owner", "new"));
    });
    require(started.wait_for(std::chrono::seconds(3)) ==
        std::future_status::ready, "same-key writer did not start");
    std::string independent = "other";
    while ((std::hash<std::string>()(independent) & 255) ==
        (std::hash<std::string>()("owner") & 255)) independent += 'x';
    std::promise<bool> other;
    auto progressed = other.get_future();
    std::thread unrelated([&] {
        other.set_value(database->set(independent, "parallel"));
    });
    require(progressed.wait_for(std::chrono::seconds(3)) ==
        std::future_status::ready && progressed.get(),
        "sync serialized unrelated key stripes");
    require(written.wait_for(std::chrono::milliseconds(0)) ==
        std::future_status::timeout, "same-key writer crossed WAL barrier");
    {
        std::lock_guard<std::mutex> guard(mutex);
        released = true; changed.notify_all();
    }
    fence.join(); writer.join(); unrelated.join();
    require(fenced.get() == db::sync_status::ok && written.get(),
        "sync/writer failed after release");
}
int main() {
    acl::acl_cpp_init();
    outcomes(); unsupported(); ordering();
    std::cout << "WAL barrier outcomes, isolation and volatile rejection "
        "passed\n";
}
