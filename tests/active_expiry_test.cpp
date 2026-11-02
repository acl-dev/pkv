#include "stdafx.h"
#include "db/expiry_index.h"
#include "db/expiry_service.h"
#include "db/database_pool.h"
#include "db/page_workspace.h"
#include "db/mdb/mdb.h"
#include "db/db_watcher.h"
#include "dao/native_import.h"
#include "coder/string_page.h"
#include <atomic>
#include <chrono>
#include <climits>
#include <cstdlib>
#include <iostream>
#include <new>
#include <thread>
#include <unordered_set>

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
void operator delete(void* p, std::size_t) noexcept { ::operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept { ::operator delete(p); }
#endif
using namespace pkv;
static void require(bool ok, const char* message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}
static uint64_t count(shared_db source) {
    uint64_t out;
    require(source->key_count(out) == db::count_status::ok, "expiry count");
    return out;
}
static std::string encoded(int64_t deadline) {
    string_root root;
    root.expires_at_ms = deadline; root.length = 5;
    root.inline_value = "value";
    std::string bytes;
    require(string_page_codec::encode_root(root, bytes) ==
        string_page_status::ok, "expiry root encode");
    return bytes;
}
static shared_db memory(bool tbb) {
    auto source = tbb ? db::create_mdb_tbb(4) : db::create_mdb(4);
    require(source->open(""), "expiry memory open"); return source;
}
static void index_checks() {
    expiry_index index;
    for (int64_t deadline = 1; deadline < 10000; ++deadline)
        require(index.set("one", deadline) && index.size() == 1,
            "expiry index grew stale deadline nodes");
    std::vector<std::string> due;
    require(!index.due(9998, 10, due) && due.empty(),
        "stale deadline survived");
    index.erase("one");
    for (unsigned i = 0; i < 2000; ++i)
        require(index.set(std::to_string(i), 1), "expiry index rehash seed");
    std::unordered_set<std::string> seen;
    while (index.size()) {
        index.due(2, 7, due);
        require(!due.empty() && due.size() <= 7, "expiry bounded candidates");
        for (const auto& key : due) {
            require(seen.insert(key).second, "duplicate expiry owner");
            index.erase(key);
        }
    }
    require(seen.size() == 2000, "expiry index key lifetime after rehash");
    for (long position = 0; position < 8; ++position) {
        expiry_index failing;
        budget = position;
        const bool ok = failing.set("key", 1);
        budget = -1;
        require(ok ? failing.size() == 1 : failing.dirty(),
            "expiry index did not report allocation loss");
        require(failing.size() <= 1, "expiry index leaked owner");
    }
}
static void cached_deadlines() {
    expiry_index index;
    std::vector<std::string> due;
    const std::string first = "first";
    std::string second;
    const auto stripe = std::hash<std::string>()(first) % 64;
    for (unsigned i = 0; second.empty(); ++i) {
        const auto key = "collision:" + std::to_string(i);
        if (std::hash<std::string>()(key) % 64 == stripe) second = key;
    }
    require(index.set(first, 10) && index.set(second, 20) &&
        index.set(second, 5), "cached expiry minimum seed");
    index.due(5, 64, due);
    require(due.size() == 1 && due[0] == second,
        "cached expiry minimum did not move backward");
    require(index.set(second, 6), "cached expiry forward seed");
    index.due(6, 64, due);
    require(due.size() == 1 && due[0] == second,
        "cached expiry minimum did not advance");
    index.erase(second);
    require(!index.due(9, 64, due) && due.empty(), "future cache was stale");
    require(index.set("", 0), "epoch/empty expiry key");
    index.due(0, 64, due);
    require(due.size() == 1 && due[0].empty(), "epoch mistaken for empty");
    index.erase(""); index.erase("first");
    require(index.set("maximum", INT64_MAX), "maximum expiry timestamp");
    index.due(INT64_MAX, 64, due);
    require(due.size() == 1 && due[0] == "maximum", "maximum deadline not due");
    index.erase("maximum");
    require(index.size() == 0 && !index.due(INT64_MAX, 64, due) && due.empty(),
        "maximum expiry mistaken for empty");
    require(index.set("old", 5), "cached expiry rollback seed");
    budget = 0;
    const bool changed = index.set("old", 10);
    budget = -1;
    require(!changed && index.dirty() && !index.due(4, 64, due) && due.empty(),
        "failed deadline update published a new minimum");
    index.due(5, 64, due);
    require(due.size() == 1 && due[0] == "old", "lost old cached deadline");
}
static void concurrent_index() {
    expiry_index index;
    std::atomic<bool> stop(false);
    std::thread reader([&] {
        std::vector<std::string> due;
        while (!stop.load()) {
            index.due(0, 32, due);
            for (const auto& key : due)
                require(key.find("worker:") == 0, "invalid cached owner");
        }
    });
    std::vector<std::thread> writers;
    for (unsigned worker = 0; worker < 4; ++worker)
        writers.emplace_back([&, worker] {
            for (unsigned i = 0; i < 2000; ++i) {
                const auto key = "worker:" + std::to_string(worker) + ':' +
                    std::to_string(i % 64);
                require(index.set(key, i % 2 ? INT64_MAX : 0),
                    "concurrent cached expiry publication");
                index.erase(key);
            }
        });
    for (auto& writer : writers) writer.join();
    stop = true; reader.join();
    require(index.size() == 0, "cached empty shortcut lost a removal");
}
static void handed_off_index() {
    expiry_index index;
    std::atomic<unsigned> turn(0);
    std::thread producer([&] {
        for (unsigned i = 0; i < 2000; ++i) {
            while (turn.load(std::memory_order_acquire) != 0)
                std::this_thread::yield();
            require(index.set("handoff", INT64_MAX), "expiry handoff set");
            turn.store(1, std::memory_order_release);
        }
    });
    std::thread consumer([&] {
        for (unsigned i = 0; i < 2000; ++i) {
            while (turn.load(std::memory_order_acquire) != 1)
                std::this_thread::yield();
            index.erase("handoff");
            turn.store(0, std::memory_order_release);
        }
    });
    producer.join(); consumer.join();
    require(index.size() == 0, "cross-thread expiry hint lost removal");
}
static void seed_types(shared_db source, bool native) {
    const std::string large(70000, 'x');
    for (unsigned type = 0; type < 5; ++type) {
        value_record record;
        record.header.kind = static_cast<value_kind>(type);
        record.header.expires_at_ms = 1;
        record.string_value = large;
        record.list_value.push_back(large);
        record.hash_value["field"] = large;
        record.set_value.insert(large);
        record.zset_value[large] = 1;
        const auto key = "expired:" + std::to_string(type);
        std::string bytes;
        require(source->value_codec().encode(record, bytes) == codec_status::ok
            && source->set(key, bytes), "expiry typed seed");
        if (!native) continue;
        require(source->transact_pages({key}, [&](page_transaction& tx) {
            const auto& root = *tx.roots().find(key);
            std::unique_ptr<record_view> view;
            dao::dao_base::read_status error;
            return source->value_codec().decode_stored(root.value, view) ==
                codec_status::ok && dao::import_native(*view, tx, key, error);
        }) == db::batch_status::ok, "expiry native import");
    }
}
static void types(shared_db source, bool native) {
    seed_types(source, native);
    require(count(source) == 5 && source->rebuild_expiry() ==
        db::scan_status::ok, "expiry initial snapshot");
    std::vector<watch_token> watched;
    require(source->watch({"expired:0"}, watched) == db::batch_status::ok,
        "expiry absent WATCH setup");
    size_t removed = 0;
    for (unsigned attempt = 0; count(source) && attempt < 10; ++attempt) {
        const auto progress = source->expire_step(2);
        require(progress.ok && progress.checked <= 2 && progress.removed <= 2,
            "expiry step budget/commit");
        removed += progress.removed;
    }
    require(removed == 5 && count(source) == 0 && watched[0]->valid(0),
        "expiry failed cleanup or invalidated absent WATCH");
    for (unsigned type = 0; native && type < 5; ++type) {
        const auto key = "expired:" + std::to_string(type);
        require(source->transact_pages({key}, [&](page_transaction& tx) {
            std::string bytes;
            return tx.lookup(key, 1, bytes) == db::lookup_status::missing;
        }) == db::batch_status::ok, "expiry orphaned native pages");
    }
}
class no_rescan : public mdb {
public:
    no_rescan() : mdb(4) {}
    bool forbid = false;
protected:
    scan_status dbscan_keys(unsigned long long cursor, size_t size,
        std::vector<std::string>& keys, unsigned long long& next) override {
        require(!forbid, "active expiry periodically scanned the keyspace");
        return mdb::dbscan_keys(cursor, size, keys, next);
    }
};
static void mutation_paths() {
    auto source = std::make_shared<no_rescan>();
    shared_db database = source;
    require(source->open("") && source->rebuild_expiry() ==
        db::scan_status::ok, "expiry mutation source");
    source->forbid = true;
    const auto bytes = encoded(1);
    require(source->set("raw", bytes) && source->replica_set("replica", bytes),
        "expiry raw publication");
    require(source->update("update", [&](bool, std::string& out) {
        out = bytes; return db::update_action::write;
    }), "expiry update publication");
    require(source->transact({"batch-a", "batch-b"}, [&](db::batch_view roots) {
        for (auto& root : roots) {
            root.value = bytes; root.action = db::update_action::write;
        }
        return true;
    }) == db::batch_status::ok, "expiry batch publication");
    require(source->transact_pages({"native"}, [&](page_transaction& tx) {
        auto& root = *tx.roots().find("native");
        root.value = bytes; root.action = db::update_action::write;
        return true;
    }) == db::batch_status::ok, "expiry native publication");
    require(source->copy_to("raw", *source, "copied", [&](bool found,
        const std::string& value, bool, std::string& out,
        db::update_action& action) {
        out = value; action = db::update_action::write; return found;
    }) == db::batch_status::ok, "expiry copy publication");
    {
        auto pending = std::make_shared<page_workspace>(database);
        require(pending->set("workspace", bytes) && pending->commit() ==
            db::batch_status::ok, "expiry workspace publication");
    }
    std::vector<db::batch_entry> roots;
    roots.emplace_back("replica-native", false, bytes);
    roots.back().action = db::update_action::write;
    require(source->replica_apply_pages(roots, {}) == db::batch_status::ok,
        "expiry native replica publication");
    require(count(source) == 9, "expiry mutation seed count");
    const auto progress = source->expire_step(64);
    require(progress.ok && progress.removed == 9 && count(source) == 0,
        "expiry lost a mutation path");
}
static void stale_candidates(shared_db source) {
    const auto future = encoded(4102444800123LL), persist = encoded(-1);
    require(source->set("future", future) && source->set("persist", persist)
        && source->set("bad", "invalid-record"), "expiry renewal seed");
    require(source->rebuild_expiry() == db::scan_status::ok,
        "expiry renewal index");
    size_t removed;
    require(source->expire_keys({"future", "persist", "bad", "missing"},
        removed) == db::batch_status::ok && removed == 0 && count(source) == 3,
        "stale expiry candidate deleted a replacement");
    std::string value;
    require(source->get("future", value) && value == future &&
        source->get("bad", value) && value == "invalid-record",
        "expiry changed future/corrupt root");
    require(source->del("future") && source->del("persist") &&
        source->del("bad"), "expiry renewal cleanup");
}
static void allocation_failures(bool tbb) {
    const auto bytes = encoded(1);
    auto control = memory(tbb);
    require(control->rebuild_expiry() == db::scan_status::ok,
        "expiry allocation index");
    budget = LONG_MAX;
    require(control->set("fault", bytes), "expiry allocation control");
    const auto allocations = LONG_MAX - budget;
    budget = -1;
    for (long position = 0; position <= allocations; ++position) {
        auto source = memory(tbb);
        require(source->rebuild_expiry() == db::scan_status::ok,
            "expiry fault source");
        bool ok = false;
        budget = position;
        try { ok = source->set("fault", bytes); }
        catch (const std::bad_alloc&) {}
        budget = -1;
        require(count(source) == (ok ? 1 : 0), "expiry changed write outcome");
        const auto progress = source->expire_step(64);
        require(progress.ok && count(source) == 0,
            "expiry failed to rebuild an incomplete index");
    }
    std::cout << "Expiry post-commit allocation positions: " << allocations + 1
        << '\n';
}
class failed_delete : public mdb {
public:
    failed_delete() : mdb(4) {}
    bool reject = false;
protected:
    bool dbdel(const std::string& key) override {
        return !reject && mdb::dbdel(key);
    }
    batch_status dbapply(const std::vector<batch_entry>& roots) override {
        return reject ? batch_status::error : mdb::dbapply(roots);
    }
};
static void rejected_cleanup() {
    auto source = std::make_shared<failed_delete>();
    require(source->open("") && source->set("a", encoded(1)) &&
        source->set("b", encoded(1)) && source->rebuild_expiry() ==
        db::scan_status::ok, "expiry rejection seed");
    source->reject = true;
    require(!source->expire_step(1).ok && count(source) == 2 &&
        !source->expire_step(2).ok && count(source) == 2,
        "rejected expiry changed roots");
    source->reject = false;
    const auto retried = source->expire_step(2);
    require(retried.ok && retried.removed == 2 && count(source) == 0,
        "rejected expiry lost its deadline entries");
}
static void rebuild_failures() {
    auto source = memory(false);
    require(source->set("a", encoded(1)) && source->set("b", encoded(1)),
        "expiry rebuild seed");
    budget = LONG_MAX;
    require(source->rebuild_expiry() == db::scan_status::ok,
        "expiry rebuild control");
    const auto allocations = LONG_MAX - budget;
    budget = -1;
    for (long position = 0; position <= allocations; ++position) {
        require(source->set("a", encoded(1)) &&
            source->set("b", encoded(1)), "expiry rebuild reset");
        budget = position;
        try { source->rebuild_expiry(); }
        catch (const std::bad_alloc&) {}
        budget = -1;
        const auto progress = source->expire_step(2);
        require(progress.ok && progress.removed == 2 && count(source) == 0,
            "failed expiry snapshot replaced the valid index");
    }
    std::cout << "Expiry rebuild allocation positions: " << allocations + 1
        << '\n';
}
static void execution_isolation() {
    auto source = memory(false);
    require(source->set("expired", encoded(1)) && source->rebuild_expiry() ==
        db::scan_status::ok, "expiry gate seed");
    std::atomic<bool> started(false), done(false);
    std::thread worker;
    {
        execution_guard transaction(true);
        worker = std::thread([&] {
            started = true;
            require(source->expire_step(1).ok, "expiry gate operation");
            done = true;
        });
        while (!started) std::this_thread::yield();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        require(!done && count(source) == 1, "expiry interleaved with EXEC");
    }
    worker.join();
    require(done && count(source) == 0, "expiry did not resume after EXEC");
}
static void service_lifecycle() {
    auto zero = memory(false);
    auto pool = std::make_shared<database_pool>(zero, 16, [](size_t) {
        return memory(false);
    });
    for (size_t interval : {size_t(0), size_t(1001)}) {
        bool rejected = false;
        try { expiry_service invalid(pool, 2, interval); }
        catch (const std::invalid_argument&) { rejected = true; }
        require(rejected, "expiry service accepted unbounded stop delay");
    }
    {
        expiry_service service(pool, 2, 10);
        auto one = pool->get(1);
        require(zero->set("zero", encoded(1)) && one->set("one", encoded(1)),
            "expiry service seed");
        for (unsigned i = 0; i < 200 && (count(zero) || count(one)); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        require(count(zero) == 0 && count(one) == 0 &&
            pool->opened().size() == 2, "expiry lazy DB/service round robin");
    }
    require(zero->set("stopped", encoded(1)), "expiry stopped service seed");
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    require(count(zero) == 1, "expiry service survived destruction");
}
static shared_db backend(const std::string& name) {
    if (name == "rdb") return db::create_rdb();
    if (name == "wdb") return db::create_wdb();
    return db::create_mdb_tbb(4);
}
static void cross_store(const std::string& name, const char* paths) {
    auto source = backend(name);
    require(source && source->open(paths), "expiry cross-store open");
    const std::string first = "key:0";
    std::string second;
    const auto shard = acl_hash_crc32(first.data(), first.size()) % 2;
    for (unsigned i = 1; second.empty(); ++i) {
        const auto key = "key:" + std::to_string(i);
        if (acl_hash_crc32(key.data(), key.size()) % 2 != shard) second = key;
    }
    require(source->set(first, encoded(1)) && source->set(second, encoded(1)),
        "expiry cross-store seed");
    const auto progress = source->expire_step(2);
    require(progress.ok && progress.removed == 2 && count(source) == 0,
        "expiry did not recheck unsupported cross-store groups");
}
int main(int argc, char** argv) {
    acl::acl_cpp_init();
    if (argc >= 3 && argc >= 4 &&
        std::string(argv[3]) == "--cross-store") {
        cross_store(argv[1], argv[2]); return 0;
    }
    if (argc >= 3) {
        auto source = backend(argv[1]);
        require(source && source->open(argv[2]), "expiry backend open");
        if (argc >= 4) {
            require(count(source) == 1, "expiry reopen retained deleted roots");
            for (unsigned type = 0; type < 5; ++type) {
                const auto key = "expired:" + std::to_string(type);
                require(source->transact_pages({key},
                    [&](page_transaction& tx) {
                    std::string bytes;
                    return tx.lookup(key, 1, bytes) ==
                        db::lookup_status::missing;
                }) == db::batch_status::ok, "expiry reopened orphan pages");
            }
            size_t removed;
            require(source->expire_keys({"kept"}, removed) ==
                db::batch_status::ok && !removed, "expiry reopen future root");
        } else {
            types(source, true); stale_candidates(source);
            require(source->set("kept", encoded(4102444800123LL)),
                "expiry reopen seed");
        }
        return 0;
    }
    index_checks(); cached_deadlines(); concurrent_index();
    handed_off_index();
    mutation_paths(); execution_isolation();
    rejected_cleanup(); rebuild_failures();
    service_lifecycle();
    for (bool tbb : {false, true}) {
#ifndef HAS_TBB
        if (tbb) continue;
#endif
        types(memory(tbb), false); types(memory(tbb), true);
        stale_candidates(memory(tbb)); allocation_failures(tbb);
    }
    std::cout << "Active expiry checks passed\n";
}
