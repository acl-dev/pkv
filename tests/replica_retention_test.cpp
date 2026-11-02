#include "stdafx.h"
#include "replica_retention_support.h"
#include "dao/json/json_scan.h"
#include <atomic>
#include <stdexcept>

static void expired_reads(retention_pair& files) {
    auto& client = *files.client;
    const auto expired_name = "$" + std::to_string(retention_key.size()) +
        "\r\n" + retention_key + "\r\n";
    require(client.run({"GET", retention_key}) == "$-1\r\n" &&
        client.run({"PTTL", retention_key}) == ":-2\r\n" &&
        client.run({"EXISTS", retention_key}) == ":0\r\n" &&
        client.run({"TYPE", retention_key}) == "+none\r\n" &&
        client.run({"RANDOMKEY"}) == expired_name &&
        client.run({"KEYS", "*"}) == "*0\r\n",
        "replica exposed expired data");
    require(client.run({"MULTI"}) == "+OK\r\n" &&
        client.run({"GET", retention_key}) == "+QUEUED\r\n" &&
        client.run({"PTTL", retention_key}) == "+QUEUED\r\n" &&
        client.run({"EXEC"}) == "*2\r\n$-1\r\n:-2\r\n",
        "read-only EXEC expiry result changed");
    require(client.run({"SET", retention_key, "forbidden"}).find(
        "-READONLY ") == 0, "retention bypassed replica write policy");
    require(!execution_replica_read(), "replica read scope leaked");
}
static void delayed(bool tbb, unsigned type) {
    retention_pair files(tbb); size_t initial;
    {
        execution_guard frozen(true);
        require(files.source->run(retention_seed(type)) ==
            (type ? ":1\r\n" : "+OK\r\n"), "retention seed command");
        require(files.source->run({"PEXPIRE", retention_key, "1"}) ==
            ":1\r\n", "retention deadline");
        initial = files.changes.batches.size();
        require(files.source->run(retention_delta(type)) ==
            (!type ? ":70000\r\n" : type == 2 ? ":2\r\n" : ":1\r\n") &&
            files.source->run({"PERSIST", retention_key}) == ":1\r\n",
            "primary delta/PERSIST before expiry");
        for (size_t i = 0; i < initial; ++i)
            require(files.changes.batches[i]->apply(*files.replica) ==
                db::batch_status::ok, "initial retention replay");
    }
    const auto before = image(files.replica, retention_key);
    require(before.found && !before.pages.empty(), "paged retention fixture");
    auto notify = files.replica->wait_for_keys({retention_key});
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    expired_reads(files);
    key_wait_event event;
    require(image(files.replica, retention_key) == before &&
        !files.effects.calls && !notify->wait(0, event),
        "replica read changed bytes/pages/effects");
    std::vector<watch_token> watched;
    require(files.replica->watch({retention_key}, watched) ==
        db::batch_status::ok && watched[0]->valid(unix_time_ms()),
        "WATCH of logically missing retained data");
    {
        replica_read_guard retained(true);
        for (size_t i = initial; i < files.changes.batches.size(); ++i)
            require(files.changes.batches[i]->apply(*files.replica) ==
                db::batch_status::ok, "trusted replay discarded by read scope");
    }
    retention_value(files.replica, type);
    require(!watched[0]->valid(unix_time_ms()) && notify->wait(0, event) &&
        !files.effects.calls, "delayed authoritative effects/observer echo");
    std::cout << "Retained type " << type << " engine " << tbb
        << " pages=" << before.pages.size() << " passed\n";
}
static void legacy_and_cache(bool tbb) {
    retention_pair files(tbb);
    value_record record; record.header.kind = value_kind::list;
    record.list_value = {retention_large}; std::string bytes;
    require(default_record_codec()->encode(record, bytes) == codec_status::ok &&
        files.replica->replica_set("legacy", bytes), "legacy retention seed");
    const auto before = image(files.replica, "legacy");
    require(files.client->run({"LLEN", "legacy"}) == ":1\r\n" &&
        image(files.replica, "legacy") == before && !files.effects.calls,
        "replica read persisted legacy import");
    require(files.source->run({"PFADD", "hll", "x"}) == ":1\r\n",
        "HLL retention seed");
    for (const auto& batch : files.changes.batches)
        require(batch->apply(*files.replica) == db::batch_status::ok,
            "HLL initial replay");
    const auto hll = image(files.replica, "hll");
    require(files.client->run({"PFCOUNT", "hll"}) == ":1\r\n" &&
        image(files.replica, "hll") == hll && !files.effects.calls,
        "replica read persisted HLL cache");
    files.client->service.replica_read_only(false);
    require(files.client->run({"LPUSH", "legacy", "new"}) == ":2\r\n" &&
        image(files.replica, "legacy").root.size() <= 16384 &&
        !image(files.replica, "legacy").pages.empty() && files.effects.calls,
        "writable replica lost explicit local write/import");
}
static void primary_and_writable(bool tbb) {
    retention_pair files(tbb);
    {
        execution_guard frozen(true);
        require(files.source->run({"SET", retention_key, "old", "PX", "1"})
            == "+OK\r\n", "primary deadline seed");
        for (const auto& batch : files.changes.batches)
            require(batch->apply(*files.replica) == db::batch_status::ok,
                "writable initial replay");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    require(files.source->run({"GET", retention_key}) == "$-1\r\n" &&
        !image(files.primary, retention_key).found,
        "primary stopped reclaiming expired data");
    files.client->service.replica_read_only(false);
    const auto before = image(files.replica, retention_key);
    require(files.client->run({"GET", retention_key}) == "$-1\r\n" &&
        image(files.replica, retention_key) == before &&
        files.client->run({"SET", retention_key, "local"}) == "+OK\r\n",
        "writable replica read/write scopes changed");
    std::string value; dao::paged_string data;
    require(data.get(files.replica, retention_key, value) && value == "local" &&
        !execution_replica_read(), "local write suppressed after replica read");
}
static void scope_lifetimes() {
    require(!execution_replica_read(), "initial read scope");
    try {
        replica_read_guard outer(true), inner(true), disabled(false);
        require(execution_replica_read(), "nested read scope");
        throw std::runtime_error("scope unwind");
    } catch (const std::runtime_error&) {}
    require(!execution_replica_read(), "exception leaked read scope");
    auto database = db::create_mdb(4);
    require(database->open(""), "scope database");
    std::vector<std::thread> threads;
    for (unsigned t = 0; t < 4; ++t) threads.emplace_back([&, t] {
        const auto key = std::to_string(t);
        go[&, key] {
            {
                replica_read_guard guard(true);
                require(database->set(key, "discarded"), "discarded write");
                acl::fiber::yield();
                require(execution_replica_read(), "yield lost replica scope");
            }
            require(!execution_replica_read(), "fiber scope unwind");
        };
        go[&, key] {
            require(!execution_replica_read() && database->set(key, "live"),
                "replica scope leaked to sibling fiber");
            acl::fiber::yield();
            require(!execution_replica_read(), "sibling changed replica scope");
        };
        acl::fiber::schedule();
        require(!execution_replica_read(), "fiber scope leaked to thread");
    });
    for (auto& thread : threads) thread.join();
    for (unsigned t = 0; t < 4; ++t) {
        std::string value;
        require(database->get(std::to_string(t), value) && value == "live",
            "replica read scope changed another actor write");
    }
    std::cout << "Replica read scopes isolate fibers/threads and unwind\n";
}
static void random_views(bool tbb) {
    retention_pair files(tbb);
    seed(files.replica, retention_key, 3, 1);
    const auto expired = image(files.replica, retention_key);
    const std::string empty, binary("live\0\xff", 6);
    require(files.replica->replica_set(empty, encoded_root(0, -1)) &&
        files.replica->replica_set(binary, encoded_root(0, -1)),
        "visible sampling seed");
    std::map<std::string, unsigned> counts{{empty, 0}, {binary, 0}};
    {
        replica_read_guard retained(true);
        dao::json_scan scan;
        for (unsigned i = 0; i < 4096; ++i) {
            std::string key; bool found = false;
            require(scan.random_key(files.replica, key, found) && found &&
                counts.count(key), "RANDOMKEY exposed an expired root");
            ++counts[key];
        }
    }
    for (const auto& count : counts)
        require(count.second > 1536 && count.second < 2560,
            "live-root reservoir strongly biased");
    counts = {{empty, 0}, {binary, 0}};
    for (unsigned i = 0; i < 4096; ++i) {
        std::string key; bool found = false;
        require(files.replica->sample_live_key(key, found) ==
            db::scan_status::ok && found && counts.count(key),
            "live-root reservoir returned retained expired name");
        ++counts[key];
    }
    for (const auto& count : counts)
        require(count.second > 1536 && count.second < 2560,
            "direct reservoir strongly biased");
    require(image(files.replica, retention_key) == expired,
        "live-root sampling reclaimed expired pages");
    require(files.replica->replica_erase(empty) &&
        files.replica->replica_erase(binary), "remove sampling live roots");
    {
        auto view = std::make_shared<page_workspace>(files.replica);
        require(view->set(binary, encoded_root(0, -1)), "reservoir overlay");
        for (unsigned i = 0; i < 64; ++i) {
            std::string key; bool found = false;
            require(view->sample_live_key(key, found) == db::scan_status::ok &&
                found && key == binary, "reservoir lost private overlay");
        }
    }
    files.client->service.replica_read_only(false);
    require(files.client->run({"MULTI"}) == "+OK\r\n" &&
        files.client->run({"SET", "live", "staged"}) == "+QUEUED\r\n",
        "sampling workspace seed");
    for (unsigned i = 0; i < 64; ++i)
        require(files.client->run({"RANDOMKEY"}) == "+QUEUED\r\n",
            "queued live-root sampling");
    std::string expected = "*65\r\n+OK\r\n";
    for (unsigned i = 0; i < 64; ++i) expected += "$4\r\nlive\r\n";
    require(files.client->run({"EXEC"}) == expected &&
        image(files.replica, retention_key) == expired,
        "replica workspace sampling lost staged visibility/retention");
    std::cout << "Retained RANDOMKEY empty/binary/workspace " << tbb
        << " passed\n";
}
static void expired_names(bool tbb) {
    for (const auto& key : {std::string(), std::string("dead\0\xff", 6)}) {
        retention_pair files(tbb);
        seed(files.replica, key, 3, 1);
        files.effects.calls = 0;
        const auto before = image(files.replica, key);
        const auto expected = "$" + std::to_string(key.size()) + "\r\n" + key +
            "\r\n";
        require(files.client->run({"GET", key}) == "$-1\r\n" &&
            files.client->run({"RANDOMKEY"}) == expected &&
            files.client->run({"KEYS", "*"}) == "*0\r\n" &&
            image(files.replica, key) == before && !files.effects.calls,
            "expired empty/binary RANDOMKEY name became nil or mutated");
    }
    std::cout << "Expired RANDOMKEY name compatibility " << tbb << " passed\n";
}
int main() {
    scope_lifetimes();
    for (bool tbb : {false, true}) {
#ifndef HAS_TBB
        if (tbb) continue;
#endif
        for (unsigned type = 0; type < 5; ++type) delayed(tbb, type);
        legacy_and_cache(tbb); primary_and_writable(tbb); random_views(tbb);
        expired_names(tbb);
    }
    std::cout << "Replica retention, delayed deltas and explicit writes "
        "passed\n";
}
