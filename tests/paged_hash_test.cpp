#include "stdafx.h"
#include "dao/paged_hash.h"
#include "dao/json/json_hash.h"
#include "dao/json/json_key.h"
#include "dao/json/json_scan.h"
#include "dao/paged_list.h"
#include "coder/json_record_codec.h"
#include "db/mdb/mdb.h"
#include "common/rank_sampling.h"
#include <cstdlib>
#include <iostream>
#include <new>
#include <random>

static thread_local long allocation_budget = -1;
void* operator new(std::size_t size) {
    if (allocation_budget == 0) throw std::bad_alloc();
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
using fields = std::map<std::string, std::string>;
static const long long deadline = 4102444800123LL;

static void require(bool ok, const char* reason) {
    if (!ok) { std::cerr << reason << '\n'; std::exit(1); }
}

class tracking_db : public mdb {
public:
    tracking_db() : mdb(4) {}
    size_t reads = 0, writes = 0, blobs = 0, largest = 0;
protected:
    lookup_status dblookup_page(const std::string& owner, uint64_t id,
        std::string& out) override {
        ++reads;
        const auto result = mdb::dblookup_page(owner, id, out);
        if (result == lookup_status::found && out.compare(0, 3, "PMB") == 0)
            ++blobs;
        return result;
    }
    batch_status dbapply_pages(const std::vector<batch_entry>& roots,
        const std::vector<page_change>& pages) override {
        writes += pages.size();
        for (const auto& page : pages) largest = std::max(largest,
            page.value.size());
        return mdb::dbapply_pages(roots, pages);
    }
};

static std::shared_ptr<tracking_db> memory() {
    auto database = std::make_shared<tracking_db>();
    require(database->open(""), "memory open");
    return database;
}

static fields contents(dao::hash& hash, shared_db database,
    const std::string& key = "hash") {
    require(hash.hgetall(database, key), "get all");
    return hash.get_fields();
}

static void root(shared_db database, size_t elements) {
    std::string bytes;
    require(database->get("hash", bytes) == (elements != 0), "root existence");
    if (!elements) return;
    map_root record;
    require(bytes.size() == 48 && map_page_codec::decode_root(bytes, record) ==
        map_page_status::ok && record.kind == map_kind::hash &&
        record.elements == elements, "whole Hash value persisted");
}

static void test_model() {
    shared_db database = memory(), reference = memory();
    dao::paged_hash pages;
    dao::json_hash json;
    std::mt19937 random(832991);
    const std::vector<std::string> names = {"", "field", "count",
        std::string("a\0b", 3), std::string(40000, 'k')};
    for (unsigned step = 0; step < 600; ++step) {
        const auto& name = names[random() % names.size()];
        const auto value = random() % 3 ? std::to_string(random() % 30) :
            std::string(20000, static_cast<char>(random()));
        const auto operation = random() % 7;
        bool a = true, b = true;
        if (operation == 0) {
            require(pages.hmset(database, "hash", {{name, value}, {"x", "7"}})
                == json.hmset(reference, "hash", {{name, value}, {"x", "7"}}),
                "HSET count");
        } else if (operation == 1) {
            bool x, y;
            a = pages.hsetnx(database, "hash", name, value, x);
            b = json.hsetnx(reference, "hash", name, value, y);
            require(x == y, "HSETNX count");
        } else if (operation == 2) {
            const std::vector<std::string> fields = {name, name, "x"};
            require(pages.hdel(database, "hash", fields) ==
                json.hdel(reference, "hash", fields), "HDEL count");
        } else if (operation == 3) {
            long long x, y;
            const auto delta = static_cast<long long>(random() % 20) - 10;
            a = pages.hincrby(database, "hash", name, delta, x);
            b = json.hincrby(reference, "hash", name, delta, y);
            if (a && b) require(x == y, "HINCRBY value");
        } else if (operation == 4) {
            std::string x, y;
            a = pages.hget(database, "hash", name, x);
            b = json.hget(reference, "hash", name, y);
            if (a && b) require(x == y, "HGET value");
        } else if (operation == 5) {
            fields x, y;
            a = pages.hmget(database, "hash", {name, "x", name}, x);
            b = json.hmget(reference, "hash", {name, "x", name}, y);
            require(x == y, "HMGET result");
        } else {
            long long x, y; bool found_x, found_y;
            a = pages.hstrlen(database, "hash", name, x, found_x);
            b = json.hstrlen(reference, "hash", name, y, found_y);
            require(x == y && found_x == found_y, "HSTRLEN result");
        }
        require(a == b && (a || pages.status() == json.status()),
            "Hash success/error semantics diverged");
        const auto actual = contents(pages, database);
        require(actual == contents(json, reference), "Hash model contents");
        root(database, actual.size());
    }
}

static void test_metadata_and_locality() {
    auto native = memory();
    shared_db database = native;
    dao::paged_hash hash;
    fields expected;
    for (unsigned i = 0; i < 2000; ++i)
        expected[std::to_string(i)] = std::string(1000, 'v');
    require(hash.hmset(database, "hash", expected) == 2000, "large Hash seed");
    native->reads = native->writes = native->blobs = 0;
    long long count;
    require(hash.hlen(database, "hash", count) && count == 2000 &&
        !native->reads && !native->writes, "HLEN read field pages");
    require(hash.hset(database, "hash", "1000", "changed") &&
        native->reads < 70 && native->writes < 5,
        "point HSET scanned or rewrote whole Hash");
    native->blobs = 0;
    require(hash.hgetall(database, "hash", false) &&
        hash.get_fields().size() == 2000 && !native->blobs,
        "HKEYS loaded value pages");
    dao::json_key keys;
    native->reads = native->writes = 0;
    require(keys.set_expiry(database, "hash", deadline, 0) &&
        keys.expiry_time(database, "hash", count) && count == deadline &&
        !native->reads && !native->writes, "Hash TTL touched member pages");
    dao::json_scan scan;
    require(scan.open_collection(database, "hash", value_kind::hash),
        "open native HSCAN");
    scan_options options; options.count = SIZE_MAX; options.no_values = true;
    scan_page page;
    native->blobs = 0;
    require(scan.collection(options, page) && page.values.size() == 2000 &&
        !page.cursor && !native->blobs, "native HSCAN NOVALUES");
    require(native->largest <= 16384, "unbounded physical Hash value");
    require(!scan.open_collection(database, "hash", value_kind::set) &&
        !scan.collection(options, page) && page.values.empty(),
        "wrong-type native scan reused a view");
}

static void test_migration_and_bad_records() {
    shared_db database = memory();
    dao::paged_hash hash;
    json_record_codec codec;
    value_record old;
    old.header.kind = value_kind::hash; old.header.expires_at_ms = deadline;
    for (unsigned i = 0; i < 100; ++i)
        old.hash_value[std::to_string(i)] = "v";
    old.hash_value["large"] = std::string(1024 * 1024, 'x');
    std::string before, after;
    require(codec.encode(old, before) == codec_status::ok &&
        database->set("hash", before), "legacy fixture");
    long long value;
    require(!hash.hincrby(database, "hash", "large", 1, value) &&
        hash.status() == dao::dao_base::read_status::invalid_hash_integer &&
        database->get("hash", after) && after == before,
        "rejected increment partially migrated Hash");
    require(database->transact_pages({"hash"}, [](page_transaction& tx) {
        std::string bytes;
        return tx.lookup("hash", 1, bytes) == db::lookup_status::missing;
    }) == db::batch_status::ok, "failed migration left orphan pages");
    require(contents(hash, database) == old.hash_value, "legacy migration");
    root(database, old.hash_value.size());
    dao::json_key keys;
    require(keys.expiry_time(database, "hash", value) && value == deadline,
        "migration TTL");
    for (const auto& bytes : {
        std::string("{\"T\":\"H\",\"E\":-1,\"D\":{\"x\":\"a\",\"x\":\"b\"}}"),
        std::string("{\"T\":\"H\",\"E\":-1,\"D\":{\"a\":\"v\",\"z\":3}}"),
        std::string("PMR\3future", 10)}) {
        require(database->set("hash", bytes) &&
            !hash.hset(database, "hash", "new", "value") &&
            database->get("hash", after) && after == bytes &&
            hash.get_fields().empty(), "invalid record rewritten");
    }
}

static void test_copy_rename_and_replacement() {
    shared_db database = memory(), target = memory();
    dao::paged_hash hash;
    dao::paged_list list;
    dao::json_key keys;
    const fields expected = {{"", ""}, {"binary", std::string(100000, 'x')}};
    require(hash.hmset(database, "hash", expected) == 2 &&
        keys.set_expiry(database, "hash", deadline, 0), "copy seed");
    long long length;
    require(list.push(target, "hash", {"old"}, true, false, length),
        "old List destination");
    bool copied;
    require(keys.copy(database, "hash", target, "hash", false, copied) &&
        !copied, "COPY without REPLACE");
    require(keys.copy(database, "hash", target, "hash", true, copied) &&
        copied && contents(hash, target) == expected &&
        keys.expiry_time(target, "hash", length) && length == deadline,
        "Hash COPY over another type");
    require(hash.hset(target, "hash", "binary", "independent") &&
        contents(hash, database) == expected, "Hash COPY aliased pages");
    require(keys.rename(database, "hash", "renamed", false, copied) &&
        copied && contents(hash, database, "renamed") == expected &&
        keys.expiry_time(database, "renamed", length) && length == deadline,
        "native Hash RENAME");
    require(keys.persist(database, "renamed") &&
        keys.expiry_time(database, "renamed", length) && length == -1,
        "native Hash PERSIST");
    require(keys.set_expiry(database, "renamed", 0, 0), "immediate expiry");
    require(hash.hlen(database, "renamed", length) && !length,
        "expired Hash retained fields");
}

static void test_allocation_rollback() {
    const fields initial = {{"base", "v"}, {"large", std::string(40000, 'o')}};
    const fields replacement = {{"large", std::string(50000, 'x')},
        {"new", "new"}};
    bool success = false;
    unsigned failures = 0;
    for (long budget = 0; budget < 1200; ++budget) {
        shared_db database = memory();
        dao::paged_hash hash;
        require(hash.hmset(database, "hash", initial) == 2, "failure seed");
        std::string before, after;
        require(database->get("hash", before), "failure snapshot");
        allocation_budget = budget;
        int result = -1;
        try { result = hash.hmset(database, "hash", replacement); }
        catch (const std::bad_alloc&) {}
        allocation_budget = -1;
        if (result >= 0) { success = true; break; }
        ++failures;
        require(database->get("hash", after) && after == before &&
            contents(hash, database) == initial, "failed HMSET changed Hash");
    }
    require(success && failures > 10, "Hash allocation sweep");
}

static void test_sample(shared_db database) {
    dao::paged_hash hash;
    dao::hash::sample_values out;
    const fields expected = {{"", ""}, {std::string("f\0x", 3), "binary"},
        {"large", std::string(50000, 'v')}};
    require(hash.sample(database, "hash", 1, true, out) && out.empty(),
        "missing Hash sample");
    require(hash.hmset(database, "hash", expected) == 3, "sample seed");
    dao::json_key keys;
    require(keys.set_expiry(database, "hash", deadline, 0), "sample TTL");
    std::string before, after;
    require(database->get("hash", before), "sample snapshot");
    set_sampling_engine().seed(17219);
    for (const long long count : {0LL, 1LL, 2LL, 3LL, 20LL, LLONG_MAX,
        -1LL, -2LL, -100LL}) {
        for (bool values : {false, true}) {
            require(hash.sample(database, "hash", count, values, out),
                "sample failed");
            const auto wanted = count < 0 ? -count : std::min(count, 3LL);
            require(out.size() == static_cast<size_t>(wanted), "sample size");
            std::set<std::string> unique;
            for (const auto& field : out) {
                const auto found = expected.find(field.first);
                require(found != expected.end() && field.second ==
                    (values ? found->second : ""), "field/value mismatch");
                unique.insert(field.first);
            }
            if (count >= 0) require(unique.size() == out.size(),
                "distinct sample repeated a field");
        }
    }
    require(database->get("hash", after) && before == after &&
        contents(hash, database) == expected, "sample changed Hash");
    long long expiry;
    require(keys.expiry_time(database, "hash", expiry) && expiry == deadline,
        "sample changed TTL");
    for (const auto& bytes : {std::string("bad"),
        std::string("PMR\3future", 10)}) {
        require(database->set("bad", bytes), "invalid sample seed");
        out = {{"stale", "stale"}};
        require(!hash.sample(database, "bad", 1, true, out) && out.empty() &&
            database->get("bad", after) && after == bytes,
            "sample changed malformed/future record");
    }
    require(keys.set_expiry(database, "hash", 0, 0) &&
        hash.sample(database, "hash", -20, true, out) && out.empty(),
        "expired Hash sampled");
}

static void test_sample_locality() {
    auto native = memory();
    shared_db database = native;
    dao::paged_hash hash;
    fields expected;
    for (unsigned i = 0; i < 1024; ++i)
        expected[std::to_string(i)] = std::string(20000, 'v');
    require(hash.hmset(database, "hash", expected) == 1024, "locality seed");
    dao::hash::sample_values out;
    for (bool values : {false, true}) {
        native->reads = native->writes = native->blobs = 0;
        require(hash.sample(database, "hash", 3, values, out) &&
            out.size() == 3 && native->reads < 150 && !native->writes &&
            native->blobs == (values ? 6 : 0), "sample read whole Hash");
    }
    native->reads = native->writes = 0;
    require(hash.sample(database, "hash", 0, true, out) && out.empty() &&
        !native->reads && !native->writes, "zero sample read pages");
}

static void test_sample_migration_rollback() {
    value_record old;
    old.header.kind = value_kind::hash;
    old.header.expires_at_ms = deadline;
    old.hash_value = {{"", ""}, {"large", std::string(40000, 'v')},
        {"other", "payload"}};
    json_record_codec codec;
    std::string before;
    require(codec.encode(old, before) == codec_status::ok, "sample encode");
    bool success = false;
    unsigned failures = 0;
    for (long budget = 0; budget < 1200; ++budget) {
        shared_db database = memory();
        require(database->set("hash", before), "migration sample seed");
        dao::paged_hash hash;
        dao::hash::sample_values out = {{"stale", "stale"}};
        set_sampling_engine().seed(31);
        allocation_budget = budget;
        bool ok = false;
        try { ok = hash.sample(database, "hash", 2, true, out); }
        catch (const std::bad_alloc&) {}
        allocation_budget = -1;
        if (ok) {
            require(out.size() == 2 && out[0].first != out[1].first,
                "migration sample result");
            for (const auto& field : out)
                require(old.hash_value.at(field.first) == field.second,
                    "migration sample value");
            root(database, 3);
            long long expiry;
            dao::json_key keys;
            require(keys.expiry_time(database, "hash", expiry) &&
                expiry == deadline, "migration sample TTL");
            success = true; break;
        }
        ++failures;
        std::string after;
        require(out.empty() && database->get("hash", after) &&
            after == before, "failed sample partially migrated Hash");
        require(database->transact_pages({"hash"}, [](page_transaction& tx) {
            std::string bytes;
            return tx.lookup("hash", 1, bytes) == db::lookup_status::missing;
        }) == db::batch_status::ok, "failed sample left pages");
    }
    require(success && failures > 10, "migration sample allocation sweep");
}

static void test_rank_sampler() {
    set_sampling_engine().seed(19851);
    for (uint64_t size = 1; size < 100; ++size) {
        rank_sampler ranks(size, false);
        std::set<uint64_t> selected;
        for (uint64_t i = 0; i < size; ++i)
            require(selected.insert(ranks.next()).second,
                "rank sampler duplicate");
        require(*selected.begin() == 0 && *selected.rbegin() == size - 1,
            "rank sampler range");
    }
    rank_sampler large(uint64_t(1) << 48, false);
    std::set<uint64_t> selected;
    for (unsigned i = 0; i < 20; ++i) selected.insert(large.next());
    require(selected.size() == 20 && *selected.rbegin() < (uint64_t(1) << 48),
        "large-population sample");
    unsigned hits[4] = {};
    rank_sampler repeated(4, true);
    for (unsigned i = 0; i < 4000; ++i) ++hits[repeated.next()];
    for (auto count : hits) require(count > 800 && count < 1200,
        "rank sampler distribution");
}

int main() {
    test_model();
    test_metadata_and_locality();
    test_migration_and_bad_records();
    test_copy_rename_and_replacement();
    test_allocation_rollback();
    test_rank_sampler();
    shared_db database = memory();
    test_sample(database);
#ifdef HAS_TBB
    database = db::create_mdb_tbb(4);
    require(database->open(""), "oneTBB sample open");
    test_sample(database);
#endif
    test_sample_locality();
    test_sample_migration_rollback();
    std::cout << "paged Hash DAO and metadata tests passed\n";
}
