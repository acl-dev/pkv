#include "stdafx.h"
#include "dao/paged_string.h"
#include "dao/json/json_string.h"
#include "dao/json/json_key.h"
#include "dao/paged_hash.h"
#include "db/mdb/mdb.h"
#include <cstdlib>
#include <iostream>
#include <new>
#include <random>
#include <thread>

static thread_local long allocation_budget = -1;
static thread_local size_t allocation_limit = SIZE_MAX;
void* operator new(std::size_t size) {
    if (allocation_budget == 0 || size > allocation_limit)
        throw std::bad_alloc();
    if (allocation_budget > 0) --allocation_budget;
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
using operation = bitmap_operation;
static const int64_t deadline = 4102444800123LL;
static void require(bool ok, const char* message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}
static shared_db memory(bool tbb) {
    auto database = tbb ? db::create_mdb_tbb(4) : db::create_mdb(4);
    require(database->open(""), "BITOP backend open");
    return database;
}
static std::string model(operation op, const std::vector<std::string>& values) {
    size_t size = 0;
    for (const auto& value : values) size = std::max(size, value.size());
    std::string result(size, '\0');
    for (size_t i = 0; i < size; ++i) for (unsigned bit = 0; bit < 8; ++bit) {
        unsigned count = 0;
        bool first = false;
        for (size_t k = 0; k < values.size(); ++k) {
            const bool set = i < values[k].size() &&
                (static_cast<unsigned char>(values[k][i]) & (1U << bit));
            if (!k) first = set;
            count += set;
        }
        bool set = false;
        switch (op) {
        case operation::conjunction: set = count == values.size(); break;
        case operation::disjunction: set = count > 0; break;
        case operation::exclusive_or: set = count % 2; break;
        case operation::invert: set = !first; break;
        case operation::difference: set = first && count == 1; break;
        case operation::reverse_difference: set = !first && count > 0; break;
        case operation::and_or: set = first && count > 1; break;
        case operation::one: set = count == 1; break;
        }
        if (set) result[i] = static_cast<char>(
            static_cast<unsigned char>(result[i]) | (1U << bit));
    }
    return result;
}
static void byte_model(bool tbb) {
    auto database = memory(tbb);
    dao::paged_string data;
    dao::json_key keys;
    std::mt19937 random(171822);
    std::map<std::string, std::string> expected;
    for (const auto& key : {"a", "b", "c"}) {
        std::string value(key[0] == 'a' ? 1024 : 20001, '\0');
        for (auto& ch : value) ch = static_cast<char>(random());
        require(data.set(database, key, value), "BITOP model seed");
        expected[key] = value;
    }
    const std::vector<std::string> names = {"a", "b", "c", "missing", ""};
    for (unsigned step = 0; step < 200; ++step) {
        const auto op = static_cast<operation>(step % 8);
        const size_t count = op == operation::invert ? 1 :
            (op == operation::difference || op == operation::reverse_difference
                || op == operation::and_or ? 2 : 1) + random() % 3;
        std::vector<std::string> inputs, values;
        for (size_t i = 0; i < count; ++i) {
            inputs.push_back(names[random() % names.size()]);
            values.push_back(expected[inputs.back()]);
        }
        const std::string target = names[random() % names.size()];
        const auto result = model(op, values);
        long long length;
        require(data.bitop(database, op, inputs, target, length) &&
            length == static_cast<long long>(result.size()), "BITOP model op");
        expected[target] = result;
        std::string actual;
        if (result.empty()) {
            require(!data.get(database, target, actual) &&
                data.status() == dao::dao_base::read_status::missing,
                "empty BITOP result must delete destination");
        } else {
            require(data.get(database, target, actual) && actual == result,
                "BITOP model result bytes");
            require(keys.expiry_time(database, target, length) && length == -1,
                "BITOP result TTL");
        }
    }
}

static void legacy_and_types(bool tbb) {
    auto database = memory(tbb);
    dao::paged_string data;
    dao::json_string old;
    dao::json_key keys;
    dao::paged_hash hashes;
    dao::set_options options;
    options.expires_at_ms = deadline;
    dao::set_result outcome;
    const std::string a(20001, '\x53'), b(40003, '\xa6');
    require(old.set(database, "legacy", a, options, outcome) &&
        data.set(database, "native", b, options, outcome), "BITOP legacy seed");
    std::string before, after, out;
    require(database->get("legacy", before), "BITOP legacy snapshot");
    long long length;
    require(hashes.hset(database, "target", "field", "value"),
        "BITOP destination hash");
    require(data.bitop(database, operation::one,
        {"legacy", "native", "legacy"}, "target", length) &&
        data.get(database, "target", out) &&
        out == model(operation::one, {a, b, a}), "BITOP duplicate ONE");
    require(database->get("legacy", after) && before == after,
        "BITOP migrated read-only legacy input");
    require(keys.expiry_time(database, "native", length) && length == deadline,
        "BITOP source TTL changed");
    require(data.bitop(database, operation::exclusive_or,
        {"native", "legacy", "native"}, "legacy", length) &&
        data.get(database, "legacy", out) &&
        out == a + std::string(b.size() - a.size(), '\0'),
        "BITOP legacy self alias");
    require(keys.expiry_time(database, "legacy", length) && length == -1,
        "BITOP alias retained TTL");
    require(hashes.hset(database, "wrong", "field", "value"),
        "BITOP wrong-type seed");
    require(!data.bitop(database, operation::conjunction,
        {"missing", "wrong"}, "target", length) && length == 0 &&
        data.status() == dao::dao_base::read_status::wrong_type,
        "BITOP short-circuited type validation");
    require(keys.set_expiry(database, "wrong", 1, 0) &&
        data.bitop(database, operation::disjunction,
            {"wrong", "native"}, "target", length) &&
        data.get(database, "target", out) && out == b,
        "BITOP expired wrong type was not missing");
}

static void rollback(bool tbb, bool legacy, bool one_page = false) {
    unsigned failures = 0;
    bool success = false;
    for (long budget = 0; budget < 1000 && !success; ++budget) {
        auto database = memory(tbb);
        dao::paged_string data;
        dao::json_string old;
        const std::string a(one_page ? 2048 : 20000, '\x55');
        const std::string b(one_page ? 4096 : 40001, '\xaa');
        require((legacy ? old.set(database, "a", a) :
            data.set(database, "a", a)) && data.set(database, "b", b),
            "BITOP rollback seed");
        std::string before, after, out;
        require(database->get("a", before), "BITOP rollback snapshot");
        long long length = 0;
        allocation_budget = budget;
        bool ok = false;
        try {
            ok = data.bitop(database, operation::exclusive_or,
                {"b", "a", "b"}, "a", length);
        } catch (const std::bad_alloc&) {}
        allocation_budget = -1;
        if (ok) {
            success = true;
            require(length == static_cast<long long>(b.size()) &&
                data.get(database, "a", out) &&
                out == a + std::string(b.size() - a.size(), '\0'),
                "BITOP rollback final alias value");
        } else {
            ++failures;
            require(length == 0 && database->get("a", after) &&
                before == after,
                "failed BITOP changed destination");
            require(database->transact_page_reads({"a"},
                [&](page_transaction& tx) {
                    const auto first = legacy ? 1 :
                        string_page_codec::pages_for(a.size()) + 1;
                    for (auto id = first; id <= string_page_codec::pages_for(b.size()); ++id)
                        if (tx.lookup("a", id, out) !=
                            db::lookup_status::missing) return false;
                    return true;
                }) == db::batch_status::ok, "failed BITOP leaked output page");
            require(data.get(database, "a", out) && out == a,
                "failed BITOP changed destination bytes");
        }
        require(data.get(database, "b", out) && out == b,
            "BITOP changed independent input");
    }
    require(success && failures > 10, "BITOP allocation sweep coverage");
}

class tracked_db : public mdb {
public:
    tracked_db() : mdb(4) {}
    size_t reads = 0, writes = 0;
protected:
    lookup_status dblookup_page(const std::string& key, uint64_t id,
        std::string& out) override {
        ++reads; return mdb::dblookup_page(key, id, out);
    }
    batch_status dbapply_pages(const std::vector<batch_entry>& roots,
        const std::vector<page_change>& pages) override {
        writes += pages.size();
        return mdb::dbapply_pages(roots, pages);
    }
};
static void bounded_output() {
    auto native = std::make_shared<tracked_db>();
    shared_db database = native;
    require(database->open(""), "BITOP tracked open");
    dao::paged_string data;
    const size_t size = 8 * 1024 * 1024;
    require(data.set(database, "a", std::string(size, '\x55')) &&
        data.set(database, "b", std::string(size, '\xaa')), "BITOP large seed");
    native->reads = native->writes = 0;
    allocation_limit = 65536;
    long long length;
    const bool ok = data.bitop(database, operation::exclusive_or,
        {"a", "b"}, "a", length);
    allocation_limit = SIZE_MAX;
    require(ok && length == static_cast<long long>(size) &&
        native->reads == 1024 && native->writes == 512,
        "BITOP materialized input/output or touched extra pages");
    require(data.bitcount(database, "a", bitmap_range(), length) &&
        length == static_cast<long long>(size * 8), "BITOP large final bytes");
}

static void transaction_contract(bool tbb) {
    auto database = memory(tbb);
    dao::paged_string data;
    require(data.set(database, "a", std::string(40000, 'x')),
        "BITOP contract seed");
    require(database->transact_pages({"a"}, [](page_transaction& tx) {
        tx.put("a", 1, "bad"); tx.put("a", 3, "bad");
        tx.replace_pages("a", 3); return true;
    }) == db::batch_status::aborted, "incomplete replacement was committed");
    require(database->transact_pages({"a"}, [](page_transaction& tx) {
        string_store store(tx, "a");
        store.replace_chunks(20000, [](uint64_t offset, size_t count,
            std::string& out) {
            out.assign(count, 'y'); return offset == 0;
        });
        return true;
    }) == db::batch_status::aborted, "ignored producer failure committed");
    require(database->transact_pages({"a"}, [](page_transaction& tx) {
        string_store store(tx, "a");
        return store.replace_chunks(20000, [](uint64_t, size_t count,
            std::string& out) { out.assign(count, 'y'); return true; });
    }) == db::batch_status::aborted, "unfinished replacement was committed");
    std::string out;
    require(data.get(database, "a", out) && out == std::string(40000, 'x'),
        "failed replacement changed data");
    require(database->transact_pages({"a"}, [](page_transaction& tx) {
        if (!tx.put("a", 99, "discard")) return false;
        string_store store(tx, "a");
        return store.replace_chunks(20000, [](uint64_t, size_t count,
            std::string& bytes) { bytes.assign(count, 'y'); return true; }) &&
            store.finish();
    }) == db::batch_status::ok, "replacement over staged edits failed");
    require(database->transact_page_reads({"a"}, [&](page_transaction& tx) {
        return tx.lookup("a", 3, out) == db::lookup_status::missing &&
            tx.lookup("a", 99, out) == db::lookup_status::missing;
    }) == db::batch_status::ok, "replacement retained old/staged tail pages");
    require(data.get(database, "a", out) && out == std::string(20000, 'y'),
        "complete replacement final data");
    require(database->transact_pages({"a"}, [](page_transaction& tx) {
        return tx.erase("a", 2);
    }) == db::batch_status::ok, "corrupt BITOP input seed");
    long long length;
    require(!data.bitop(database, operation::invert, {"a"}, "a", length),
        "BITOP ignored missing input page");
    require(data.getrange(database, "a", 0, 10, out) &&
        out == std::string(11, 'y'),
        "BITOP failure published earlier output pages");
    require(database->get("a", out), "BITOP future snapshot");
    out[3] = 99;
    require(database->set("future", out), "BITOP future seed");
    require(!data.bitop(database, operation::invert, {"future"}, "a", length)
        && !length && data.status() ==
            dao::dao_base::read_status::unsupported_format,
        "BITOP accepted future input format");
    require(data.set(database, "good", "z") &&
        data.bitop(database, operation::disjunction, {"good"}, "future",
            length) && length == 1 && data.get(database, "future", out) &&
        out == "z", "BITOP could not replace an opaque non-input target");
}

static void concurrent(bool tbb) {
    auto database = memory(tbb);
    dao::paged_string data;
    const std::string zero(20001, '\0'), mask(20001, '\xff');
    require(data.set(database, "a", zero) && data.set(database, "mask", mask),
        "BITOP concurrent seed");
    std::vector<std::thread> threads;
    for (unsigned n = 0; n < 6; ++n) threads.emplace_back([&] {
        dao::paged_string local;
        for (unsigned i = 0; i < 60; ++i) {
            long long length;
            require(local.bitop(database, operation::exclusive_or,
                {"a", "mask"}, "a", length) && length == 20001,
                "concurrent BITOP alias update");
        }
    });
    for (auto& thread : threads) thread.join();
    std::string out;
    require(data.get(database, "a", out) && out == zero,
        "concurrent BITOP lost atomic updates");
}

static void disk_paths(const std::string& backend, const std::string& path) {
    auto database = backend == "rdb" ? db::create_rdb() : db::create_wdb();
    const auto paths = path + "/left;" + path + "/right";
    require(database->open(paths.c_str()), "BITOP multi-path open");
    const auto shard = [](const std::string& key) {
        return acl_hash_crc32(key.data(), key.size()) % 2;
    };
    std::string first("a"), second("b");
    while (shard(first) == shard(second)) second += "x";
    dao::paged_string data;
    dao::json_string old;
    const std::string a(20000, '\x55'), b(40001, '\xaa');
    require(old.set(database, first, a) && data.set(database, second, b),
        "BITOP multi-path inputs");
    std::string before, after;
    require(database->get(first, before), "BITOP multi-path snapshot");
    long long length;
    require(data.bitop(database, operation::disjunction,
        {first, second}, "target", length) && length == 40001 &&
        data.get(database, "target", after) &&
        after == model(operation::disjunction, {a, b}),
        "BITOP rejected read-only inputs from separate paths");
    require(database->get(first, after) && after == before,
        "BITOP multi-path migrated source");
    require(data.bitop(database, operation::one,
        {first, second, first}, first, length) &&
        data.get(database, first, after) &&
        after == model(operation::one, {a, b, a}) &&
        data.get(database, second, after) && after == b,
        "BITOP multi-path alias result");
}

static void disk(const std::string& backend, const std::string& path) {
    const std::string key("bitop\0key", 9), a(20001, '\x55'), b(40003, '\xaa');
    for (unsigned phase = 0; phase < 3; ++phase) {
        auto database = backend == "rdb" ? db::create_rdb() : db::create_wdb();
        require(database->open(path.c_str()), "BITOP disk open");
        dao::paged_string data;
        dao::json_key keys;
        long long length;
        if (!phase) {
            require(data.set(database, key, a) && data.set(database, "b", b) &&
                keys.set_expiry(database, key, deadline, 0) &&
                data.bitop(database, operation::exclusive_or,
                    {key, "b"}, key, length), "BITOP disk seed");
        } else {
            std::string out;
            const auto expected = model(phase == 1 ? operation::exclusive_or :
                operation::reverse_difference, {a, b});
            require(data.get(database, key, out) && out == expected &&
                keys.expiry_time(database, key, length) && length == -1,
                "BITOP disk retained data/TTL");
            if (phase == 1) {
                require(data.set(database, key, a) &&
                    data.bitop(database, operation::reverse_difference,
                        {key, "b"}, key, length), "BITOP disk overwrite");
            }
        }
    }
    disk_paths(backend, path);
}

int main(int argc, char** argv) {
    acl::acl_cpp_init();
    if (argc == 4 && std::string(argv[1]) == "--disk") {
        disk(argv[2], argv[3]); return 0;
    }
    for (bool tbb : {false, true}) {
#ifndef HAS_TBB
        if (tbb) continue;
#endif
        byte_model(tbb); legacy_and_types(tbb); transaction_contract(tbb);
        rollback(tbb, false); rollback(tbb, true); concurrent(tbb);
        rollback(tbb, false, true); rollback(tbb, true, true);
    }
    bounded_output();
    std::cout << "BITOP model/alias/rollback/stream/concurrency passed\n";
}
