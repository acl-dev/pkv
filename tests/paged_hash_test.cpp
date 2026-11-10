#include "stdafx.h"
#include "coder/compact_hash.h"
#include "dao/paged_hash.h"
#include "dao/json/json_hash.h"
#include "dao/json/json_key.h"
#include "dao/json/json_scan.h"
#include "dao/paged_list.h"
#include "coder/json_record_codec.h"
#include "db/mdb/mdb.h"
#ifdef HAS_TBB
#include "db/mdb/mdb_tbb.h"
#endif
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
    if (compact_hash::matches(bytes)) {
        require(compact_hash::view(bytes).count() == elements, "compact Hash size");
        return;
    }
    map_root record;
    require(map_page_codec::decode_root(bytes, record) ==
        map_page_status::ok && record.kind == map_kind::hash &&
        record.elements == elements, "whole Hash value persisted");
}

static void test_single_write(bool tbb) {
    dao::paged_hash hash;
    dao::json_key keys;
    const std::string field("field\0binary", 12), value(40000, 'v');
    shared_db database = tbb ? db::create_mdb_tbb(4) : memory();
    require(database && database->open(""), "single write open");
    bool added = false;
    require(hash.hset(database, "single", field, value, added) && added &&
        keys.set_expiry(database, "single", deadline, 0), "single add/TTL");
    added = true;
    require(hash.hset(database, "single", field, "next", added) && !added,
        "single replace count");
    long long expiry;
    require(keys.expiry_time(database, "single", expiry) &&
        expiry == deadline, "single replace lost TTL");
    require(database->set("bad", "invalid"), "single wrong record seed");
    added = true;
    require(!hash.hset(database, "bad", field, value, added) && !added,
        "single failed write published count");
    for (bool insert : {false, true}) {
        bool success = false;
        unsigned failures = 0;
        for (long budget = 0; budget < 1200; ++budget) {
            database = tbb ? db::create_mdb_tbb(4) : memory();
            require(database && database->open("") &&
                hash.hset(database, "hash", "old", "before"),
                "single rollback seed");
            std::string before, after;
            require(database->get("hash", before), "single snapshot");
            added = true;
            bool ok = false;
            allocation_budget = budget;
            try {
                ok = hash.hset(database, "hash", insert ? field : "old",
                    value, added);
            } catch (const std::bad_alloc&) {}
            allocation_budget = -1;
            if (ok) {
                require(added == insert, "single committed count");
                success = true; break;
            }
            ++failures;
            require(!added && database->get("hash", after) &&
                after == before && contents(hash, database) ==
                    fields{{"old", "before"}}, "single rollback changed Hash");
        }
        require(success && failures > 0, "single rollback sweep");
    }
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

    for (const uint64_t size : {uint64_t(8), uint64_t(1) << 48}) {
        std::array<uint64_t, 8> ranks;
        size_t count = 0;
        allocation_budget = 0;
        const bool ok = visit_sampled_ranks(size, ranks.size(), false,
            [&](uint64_t rank) { ranks[count++] = rank; return true; });
        allocation_budget = -1;
        require(ok && count == ranks.size(), "small rank sample count");
        std::sort(ranks.begin(), ranks.end());
        require(ranks.back() < size &&
            std::adjacent_find(ranks.begin(), ranks.end()) == ranks.end(),
            "small rank sample allocated or returned an invalid rank");
    }
}

static void test_compact_owned_update() {
    std::vector<fields> models{{{"", ""}, {"a", "one"}, {"b", "two"}},
        {{std::string("a\0", 2), std::string("v\0x", 3)}, {"z", "end"}}};
    fields large;
    for (unsigned i = 0; i < 63; ++i)
        large.emplace("field:" + std::to_string(1000000000000ULL + i), "val");
    models.push_back(large);
    for (const auto& original : models) {
        for (const auto& field : original) {
            auto bytes = compact_hash::encode(original, deadline);
            require(!bytes.empty(), "owned Hash fixture");
            const auto* address = bytes.data();
            const auto capacity = bytes.capacity();
            const std::string value(field.second.size(), '\0');
            bool added = true;
            allocation_budget = 0;
            bool ok = false;
            try { ok = compact_hash::view(bytes).put(field.first, value, bytes, added); }
            catch (const std::bad_alloc&) {}
            allocation_budget = -1;
            require(ok && !added && bytes.data() == address &&
                bytes.capacity() == capacity, "owned equal update allocated");
            const compact_hash::view result(bytes);
            require(result.count() == original.size() && result.deadline() == deadline,
                "owned equal update metadata");
            for (const auto& expected : original) {
                std::string actual;
                require(result.get(expected.first, actual) && actual ==
                    (expected.first == field.first ? value : expected.second),
                    "owned equal update changed another field");
            }
        }
    }
    auto bytes = compact_hash::encode({{"a", "one"}, {"b", "two"}}, deadline);
    const compact_hash::view borrowed(bytes);
    bool added;
    require(borrowed.put(borrowed.at(0).first, borrowed.at(1).second, bytes, added)
        && !added, "owned borrowed replacement");
    std::string actual;
    require(compact_hash::view(bytes).get(std::string("a"), actual) && actual == "two",
        "owned borrowed replacement bytes");
    for (const std::string value : {std::string(254, 'x'), std::string(), std::string("next")}) {
        require(compact_hash::view(bytes).put(std::string("a"), value, bytes, added) &&
            !added && compact_hash::view(bytes).get(std::string("a"), actual) &&
            actual == value && compact_hash::view(bytes).deadline() == deadline,
            "owned resizing fallback");
    }
    require(compact_hash::view(bytes).put(std::string("new"), std::string("value"),
        bytes, added) && added, "owned insertion fallback");
    const auto before = bytes;
    const std::string longer(254, 'y');
    bool failed = false;
    allocation_budget = 0;
    try { compact_hash::view(bytes).put(std::string("a"), longer, bytes, added); }
    catch (const std::bad_alloc&) { failed = true; }
    allocation_budget = -1;
    require(failed && bytes == before, "owned OOM changed encoded root");
    require(!compact_hash::view(bytes).put(std::string("a"), std::string(255, 'z'),
        bytes, added) && bytes == before, "owned rejected update changed root");
}

static void test_compact_owned_transaction(bool tbb) {
    shared_db database = tbb ? db::create_mdb_tbb(4) : memory();
    require(database && database->open(""), "owned transaction database");
    const auto before = compact_hash::encode({{"a", "one"}, {"b", "two"}}, deadline);
    require(database->set("hash", before), "owned transaction seed");
    std::vector<watch_token> tokens;
    require(database->watch({"hash"}, tokens) == db::batch_status::ok,
        "owned transaction WATCH");
    require(database->transact_page("hash", [&](page_transaction& tx) {
        auto& root = *tx.roots().find("hash");
        bool added;
        require(compact_hash::view(root.value).put(std::string("a"),
            std::string("new"), root.value, added) && !added,
            "owned aborted transaction update");
        root.action = db::update_action::write;
        return false;
    }) == db::batch_status::aborted, "owned transaction must abort");
    std::string after;
    require(database->get("hash", after) && after == before &&
        tokens[0]->valid(unix_time_ms()), "owned aborted update escaped working copy");
    {
        const std::string key = "hash";
        db::page_lock_scope scope(*database, key);
        dao::paged_hash hash(&scope);
        bool added = true;
        require(hash.hset(database, key, "a", "one", added) && !added,
            "owned scoped equal value write");
        require(!tokens[0]->valid(unix_time_ms()),
            "owned same-value write did not invalidate WATCH");
        require(hash.hset(database, key, "a", "new", added) && !added,
            "owned scoped replacement");
    }
    require(database->get("hash", after) && !tokens[0]->valid(unix_time_ms()),
        "owned committed write did not invalidate WATCH");
    const compact_hash::view result(after);
    std::string value;
    require(result.deadline() == deadline && result.count() == 2 &&
        result.get(std::string("a"), value) && value == "new" &&
        result.get(std::string("b"), value) && value == "two",
        "owned scoped commit changed metadata or sibling");
}

template <class Backend>
class yielding_hash_db : public Backend {
public:
    yielding_hash_db() : Backend(8) {}
    size_t root_yields = 0, page_yields = 0;
protected:
    db::lookup_status dblookup(const std::string& key, std::string& out) override {
        ++root_yields; acl::fiber::yield();
        return Backend::dblookup(key, out);
    }
    bool dbget(const std::string& key, std::string& out) override {
        ++root_yields; acl::fiber::yield();
        return Backend::dbget(key, out);
    }
    db::lookup_status dblookup_bounded_root(const std::string& key,
        std::string& out) override {
        ++root_yields; acl::fiber::yield();
        return Backend::dblookup_bounded_root(key, out);
    }
    bool dbbind_page_reader(const std::string&, db::page_reader&) override {
        return false;
    }
    db::lookup_status dbinspect_page(const std::string& owner, uint64_t id,
        std::string& scratch, db::page_inspection& inspection) override {
        ++page_yields; acl::fiber::yield();
        return Backend::dbinspect_page(owner, id, scratch, inspection);
    }
};

template <class Backend>
static void test_hash_callback_suspension() {
    auto native = std::make_shared<yielding_hash_db<Backend>>();
    shared_db database = native;
    require(database->open(""), "yielding Hash database");
    for (unsigned owner = 0; owner < 8; ++owner) go[&, owner] {
        dao::paged_hash hash;
        dao::json_key keys;
        for (unsigned count : {3U, 63U, 129U}) {
            const auto key = "yield:" + std::to_string(owner) + ":" + std::to_string(count);
            fields expected;
            for (unsigned i = 0; i < count; ++i)
                expected.emplace("f" + std::to_string(1000 + i),
                    std::string(3, static_cast<char>('a' + owner)));
            require(hash.hmset(database, key, expected) == static_cast<int>(count) &&
                keys.set_expiry(database, key, deadline, 0), "yielding Hash seed/TTL");
            if (count == 63) {
                const std::string a = "new-a", b = "new-b", value(3, 'z');
                require(hash.hmset_pair(database, key, {&a, &value}, {&b, &value}) == 2,
                    "yielding Hash promotion callback");
                expected[a] = expected[b] = value;
            }
            const auto first = expected.begin()->first, last = expected.rbegin()->first;
            for (unsigned round = 0; round < 8; ++round) {
                std::string value;
                require(hash.hget(database, key, first, value) &&
                    value == expected.at(first), "Hash read callback crossed fibers");
                const std::string next{static_cast<char>(owner), '\0', static_cast<char>(round)};
                bool added;
                require(hash.hset(database, key, first, next, added) && !added,
                    "Hash write callback crossed fibers");
                expected[first] = next;
                fields result;
                require(hash.hmget(database, key, {first, last}, result) &&
                    result == fields{{first, next}, {last, expected.at(last)}},
                    "Hash multi-read callback crossed fibers");
                require(hash.hset(database, key, "take", next, added) && added,
                    "yielding Hash temporary field");
                dao::hash_field_values removed;
                require(hash.hgetdel(database, key, [](std::vector<std::string>& names) {
                    names = {"take", "take"}; return true;
                }, removed) && removed.size() == 2 && removed[0].found &&
                    removed[0].value == next && !removed[1].found,
                    "Hash remove callback crossed fibers");
                dao::hash::sample_values samples;
                require(hash.sample(database, key, 2, true, samples) && samples.size() == 2,
                    "yielding Hash sample");
                for (const auto& field : samples)
                    require(expected.at(field.first) == field.second,
                        "Hash sample callback crossed fibers");
                require(hash.hgetall(database, key) && hash.get_fields() == expected,
                    "Hash enumeration callback crossed fibers");
            }
            long long expires;
            require(keys.expiry_time(database, key, expires) && expires == deadline,
                "yielding Hash lost TTL");
        }
    };
    acl::fiber::schedule();
    require(native->root_yields > 0 && native->page_yields > 0,
        "Hash callback suspension hooks were not exercised");
}

int main() {
    acl::acl_cpp_init();
    test_compact_owned_update();
    test_compact_owned_transaction(false);
#ifdef HAS_TBB
    test_compact_owned_transaction(true);
#endif
    {
        shared_db database = memory();
        dao::paged_hash hash;
        dao::json_key keys;
        auto fixed = compact_hash::encode({{"abc", "one"}, {"def", "two"}});
        std::string lookup;
        require(compact_hash::view(fixed).get(std::string("def"), lookup) &&
            lookup == "two", "fixed-width Hash lookup");
        const auto second_field = fixed.find("def");
        require(second_field != std::string::npos, "fixed-width Hash fixture");
        fixed.replace(second_field, 3, "abc");
        require(database->set("bad-fixed", fixed) &&
            !hash.hget(database, "bad-fixed", "abc", lookup) &&
            hash.status() == dao::dao_base::read_status::invalid_record,
            "fixed-width Hash accepted duplicate field");
        const auto mixed = compact_hash::encode({{"a", "12345"}, {"def", "two"}});
        require(compact_hash::view(mixed).get(std::string("a"), lookup) &&
            lookup == "12345", "equal-width Hash with variable field lengths");
        const std::string large(254, 'x');
        const auto narrow = compact_hash::encode({{"a", large}});
        std::string wide;
        bool widened;
        require(compact_hash::view(narrow).put(std::string("b"), large, wide,
            widened) && widened &&
            (compact_hash::byte(wide[4]) & compact_hash::wide_offsets_flag),
            "256-byte Hash should widen at 512 bytes");
        std::map<std::string, std::string> wide_fields;
        for (char field = 'a'; field <= 'h'; ++field)
            wide_fields.emplace(std::string(1, field), large);
        wide = compact_hash::encode(wide_fields);
        compact_hash::view(wide).replace_deadline(wide, deadline);
        const compact_hash::view packed(wide);
        require(packed.deadline() == deadline &&
            packed.get(std::string("b"), lookup) && lookup == large,
            "wide Hash deadline and lookup");
        const auto remaining = packed.without(1, 7);
        require(compact_hash::view(remaining).get(std::string("b"), lookup) &&
            lookup == large &&
            !packed.put(std::string("i"), std::string(), wide, widened),
            "wide Hash removal and overflow");
        std::map<std::string, std::string> many;
        for (unsigned i = 0; i < compact_hash::max_fields; ++i)
            many.emplace("f" + std::to_string(10 + i), "val");
        const auto last = many.rbegin()->first;
        auto full = compact_hash::encode(many, deadline);
        require(database->set("full", full) &&
            hash.hdel(database, "full", std::vector<std::string>{last, last}) == 1 &&
            database->get("full", full) &&
            compact_hash::view(full).count() == many.size() - 1 &&
            !compact_hash::view(full).get(last, lookup), "compact Hash removal above bit 31");
        bool restored;
        require(hash.hset(database, "full", last, "val", restored) && restored,
            "refill the compact Hash");
        many.emplace("overflow", "val");
        require(hash.hmset(database, "full", many) == 1 &&
            database->get("full", full) && !compact_hash::matches(full),
            "large batch promotes the compact Hash");
        const std::string binary("\0\xff", 2), empty, other = "other";
        std::string bytes, value;
        bool added;
        require(hash.hset(database, "small", empty, binary, added) && added &&
            database->get("small", bytes) && compact_hash::matches(bytes),
            "compact single-field Hash");
        require(keys.set_expiry(database, "small", deadline, 0) &&
            hash.hmset_pair(database, "small", {&binary, &empty},
                {&empty, &other}) == 1 &&
            database->get("small", bytes) && compact_hash::matches(bytes) &&
            compact_hash::view(bytes).deadline() == deadline &&
            hash.hget(database, "small", binary, value) && value.empty() &&
            hash.hget(database, "small", empty, value) && value == other,
            "compact two-field update and TTL");
        long long length;
        bool exists;
        fields values;
        std::string after;
        require(hash.hlen(database, "small", length) && length == 2 &&
            hash.hstrlen(database, "small", binary, length, exists) &&
            exists && length == 0 &&
            hash.hstrlen(database, "small", "missing", length, exists) &&
            !exists && length == 0 &&
            hash.hmget(database, "small", {binary, empty, "missing", binary}, values) &&
            values == fields{{binary, empty}, {empty, other}} &&
            database->get("small", after) && after == bytes,
            "compact Hash lookups touched storage");
        dao::paged_hash::field_list entries;
        require(hash.hgetall(database, "small", true, entries) &&
            entries == dao::paged_hash::field_list{{empty, other}, {binary, empty}} &&
            hash.hgetall(database, "small", false, entries) &&
            entries == dao::paged_hash::field_list{{empty, empty}, {binary, empty}} &&
            hash.hgetall(database, "small") && hash.get_fields() == values,
            "compact Hash enumeration");
        dao::json_scan scanner;
        scan_options options;
        options.has_match = true;
        options.pattern = "?*";
        options.no_values = true;
        scan_page page;
        require(scanner.open_collection(database, "small", value_kind::hash) &&
            scanner.collection(options, page) && page.cursor == 0 &&
            page.values == std::vector<std::string>{binary} &&
            database->get("small", after) && after == bytes,
            "compact Hash NOVALUES scan touched storage");
        dao::hash::sample_values sampled;
        require(hash.sample(database, "small", -5, true, sampled) &&
            sampled.size() == 5, "compact Hash sampling with replacement");
        for (const auto& field : sampled)
            require(values.at(field.first) == field.second, "compact sampled value");
        require(hash.sample(database, "small", 3, false, sampled) &&
            sampled.size() == 2 && sampled[0].first != sampled[1].first &&
            sampled[0].second.empty() && sampled[1].second.empty() &&
            database->get("small", after) && after == bytes,
            "compact Hash sampling touched storage");
        require(hash.hdel(database, "small", std::vector<std::string>{
            binary, binary, "missing"}) == 1 &&
            database->get("small", bytes) && compact_hash::matches(bytes) &&
            compact_hash::view(bytes).deadline() == deadline &&
            hash.hget(database, "small", empty, value) && value == other &&
            hash.hdel(database, "small", binary) == 0 &&
            database->get("small", after) && after == bytes,
            "compact Hash deletion and TTL");
        require(hash.hdel(database, "small", empty) == 1 &&
            !database->get("small", after), "compact Hash final deletion");
        require(hash.hsetnx(database, "small", "count", "9223372036854775807", added) &&
            added && keys.set_expiry(database, "small", deadline, 0) &&
            database->get("small", bytes) && compact_hash::matches(bytes),
            "compact conditional write");
        long long number;
        require(hash.hsetnx(database, "small", "count", "1", added) && !added &&
            !hash.hincrby(database, "small", "count", 1, number) &&
            database->get("small", after) && after == bytes,
            "compact conditional no-op and increment overflow");
        require(hash.hincrby(database, "small", "count", -1, number) &&
            number == LLONG_MAX - 1 &&
            hash.hincrby(database, "small", binary, -3, number) && number == -3 &&
            database->get("small", after) && compact_hash::matches(after) &&
            compact_hash::view(after).deadline() == deadline,
            "compact increments preserve TTL");
        require(hash.hset(database, "small", "count", "9", added) && !added &&
            hash.hincrby(database, "small", "count", 1, number) && number == 10 &&
            hash.hget(database, "small", binary, value) && value == "-3" &&
            hash.hset(database, "small", empty, other, added) && added &&
            hash.hget(database, "small", empty, value) && value == other &&
            database->get("small", after) && compact_hash::matches(after) &&
            compact_hash::view(after).deadline() == deadline,
            "compact Hash offsets after shorter, longer and inserted fields");
        require(hash.hincrbyfloat(database, "small", "count", "0.25", value) &&
            value == "10.25" &&
            hash.hincrbyfloat(database, "small", "new-float", "-0.25", value) &&
            value == "-0.25" &&
            database->get("small", bytes) && compact_hash::matches(bytes) &&
            compact_hash::view(bytes).deadline() == deadline,
            "compact float increments preserve TTL");
        require(!hash.hincrbyfloat(database, "small", empty, "1", value) &&
            hash.status() == dao::dao_base::read_status::invalid_hash_float &&
            database->get("small", after) && after == bytes,
            "invalid compact float leaves the root intact");
    }
    test_single_write(false);
#ifdef HAS_TBB
    test_single_write(true);
#endif
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
    test_hash_callback_suspension<mdb>();
#ifdef HAS_TBB
    test_hash_callback_suspension<mdb_tbb>();
#endif
    std::cout << "paged Hash DAO and metadata tests passed\n";
}
