#include "stdafx.h"
#include "dao/paged_set.h"
#include "dao/json/json_set.h"
#include "dao/json/json_zset.h"
#include "dao/json/json_key.h"
#include "dao/json/json_scan.h"
#include "coder/json_record_codec.h"
#include "db/mdb/mdb.h"
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
static const long long deadline = 4102444800123LL;
static void require(bool ok, const char* reason) {
    if (!ok) { std::cerr << reason << '\n'; std::exit(1); }
}

class tracking_db : public mdb {
public:
    tracking_db() : mdb(4) {}
    size_t reads = 0, writes = 0, largest = 0;
protected:
    lookup_status dblookup_page(const std::string& key, uint64_t id,
        std::string& out) override {
        ++reads;
        return mdb::dblookup_page(key, id, out);
    }
    batch_status dbapply_pages(const std::vector<batch_entry>& roots,
        const std::vector<page_change>& pages) override {
        writes += pages.size();
        for (const auto& page : pages)
            largest = std::max(largest, page.value.size());
        return mdb::dbapply_pages(roots, pages);
    }
};

static std::shared_ptr<tracking_db> memory() {
    auto database = std::make_shared<tracking_db>();
    require(database->open(""), "memory open");
    return database;
}

static std::set<std::string> contents(dao::set& data, shared_db database,
    const std::string& key) {
    std::set<std::string> out;
    require(data.members(database, key, out), "members");
    return out;
}

static void root(shared_db database, const std::string& key, size_t size) {
    std::string bytes;
    require(database->get(key, bytes) == (size != 0), "root existence");
    if (!size) return;
    map_root record;
    require(bytes.size() == 48 && map_page_codec::decode_root(bytes, record) ==
        map_page_status::ok && record.kind == map_kind::set &&
        record.elements == size, "Set stored as whole value");
}

static void test_model() {
    shared_db database = memory(), reference = memory();
    dao::paged_set pages;
    dao::json_set json;
    std::mt19937 random(937425);
    const std::vector<std::string> keys = {"a", "b", "c"};
    const std::vector<std::string> members = {"", "x", "y", "z",
        std::string("a\0b", 3), std::string(40000, 'm')};
    for (unsigned step = 0; step < 600; ++step) {
        const auto& key = keys[random() % keys.size()];
        const auto& other = keys[random() % keys.size()];
        const auto& member = members[random() % members.size()];
        const unsigned command = random() % 7;
        long long actual, expected;
        if (command < 2) {
            const bool add = command == 0;
            require(pages.change(database, key, {member, member}, add,
                actual) && json.change(reference, key, {member, member}, add,
                expected) && actual == expected, "change model");
        } else if (command == 2) {
            bool moved, wanted;
            require(pages.move(database, key, other, member, moved) &&
                json.move(reference, key, other, member, wanted) &&
                moved == wanted, "move model");
        } else if (command == 3) {
            std::vector<long long> actual, expected;
            require(pages.membership(database, key, members, actual) &&
                json.membership(reference, key, members, expected) &&
                actual == expected, "membership model");
        } else if (command == 4) {
            const auto operation = static_cast<dao::set_operation>(
                random() % 3);
            const std::vector<std::string> sources = {key, other,
                keys[random() % keys.size()]};
            const std::string* target = random() % 2 ? &key : nullptr;
            std::set<std::string> actual, expected;
            require(pages.algebra(database, sources, operation, target,
                actual) && json.algebra(reference, sources, operation,
                target, expected) && actual == expected, "algebra model");
        } else if (command == 5) {
            const long long limit = random() % 4;
            require(pages.intersection_size(database, {key, other, key},
                limit, actual) && json.intersection_size(reference,
                {key, other, key}, limit, expected) && actual == expected,
                "intersection model");
        } else {
            require(pages.size(database, key, actual) &&
                json.size(reference, key, expected) && actual == expected,
                "cardinality model");
        }
        for (const auto& name : keys) {
            auto expected = contents(json, reference, name);
            require(contents(pages, database, name) == expected,
                "snapshot model");
            root(database, name, expected.size());
        }
    }
}

static void test_sampling_and_locality() {
    auto tracked = memory();
    shared_db database = tracked;
    dao::paged_set data;
    std::vector<std::string> members;
    for (unsigned i = 0; i < 10000; ++i) members.push_back(std::to_string(i));
    long long count;
    require(data.change(database, "set", members, true, count) &&
        count == 10000 && tracked->largest <= 16384, "bounded large seed");
    tracked->reads = tracked->writes = 0;
    require(data.size(database, "set", count) && count == 10000 &&
        tracked->reads == 0 && tracked->writes == 0, "root-only SCARD");
    dao::set_sample_result out;
    require(data.sample(database, "set", 1, false, out) && out.found &&
        out.values.size() == 1 && tracked->reads < 100,
        "single sample must not traverse full set");
    set_sampling_engine().seed(7739);
    require(data.sample(database, "set", 120, false, out) &&
        out.values.size() == 120 && std::set<std::string>(out.values.begin(),
            out.values.end()).size() == 120, "unique sample");
    require(data.sample(database, "set", -120, false, out) &&
        out.values.size() == 120, "repeat sample");
    require(data.sample(database, "set", 130, true, out) &&
        out.values.size() == 130 && data.size(database, "set", count) &&
        count == 9870, "native pop");
    std::vector<long long> present;
    require(data.membership(database, "set", out.values, present) &&
        std::count(present.begin(), present.end(), 0) == 130,
        "pop retained sampled member");
    require(data.sample(database, "set", 0, true, out) && out.found &&
        out.values.empty(), "zero pop");
    require(data.sample(database, "set", LLONG_MIN, false, out) == false &&
        out.values.empty() && !out.found, "minimum signed count");
    require(data.sample(database, "set", LLONG_MAX, true, out) &&
        out.values.size() == 9870, "pop all");
    root(database, "set", 0);
    require(data.sample(database, "set", -3, false, out) && !out.found &&
        out.values.empty(), "missing sample");
}

static void test_migration_scan_copy() {
    shared_db database = memory(), destination = memory();
    json_record_codec codec(1);
    value_record record;
    record.header.kind = value_kind::set;
    record.header.expires_at_ms = deadline;
    for (unsigned i = 0; i < 1001; ++i)
        record.set_value.insert(std::to_string(i));
    record.set_value.insert(std::string(1024 * 1024, 'x'));
    std::string bytes;
    require(codec.encode(record, bytes) == codec_status::ok &&
        database->set("set", bytes), "legacy seed");
    dao::paged_set data;
    long long count;
    require(data.size(database, "set", count) && count == 1002,
        "legacy migration");
    root(database, "set", 1002);
    map_root metadata;
    require(database->get("set", bytes) && map_page_codec::decode_root(bytes,
        metadata) == map_page_status::ok &&
        metadata.expires_at_ms == deadline, "migration TTL");
    dao::json_scan scanner;
    scan_options options;
    options.count = 3;
    std::set<std::string> seen;
    do {
        scan_page page;
        require(scanner.open_collection(database, "set", value_kind::set) &&
            scanner.collection(options, page), "native SSCAN");
        seen.insert(page.values.begin(), page.values.end());
        options.cursor = page.cursor;
    } while (options.cursor);
    require(seen == record.set_value, "scan missed native members");
    dao::json_key key;
    bool copied;
    require(key.copy(database, "set", destination, "copy", true, copied)
        && copied && contents(data, destination, "copy") == record.set_value,
        "independent cross-DB copy");
    require(data.change(destination, "copy", {"new"}, true, count) &&
        !contents(data, database, "set").count("new"), "copy alias");
    bool renamed;
    require(key.rename(destination, "copy", "renamed", false, renamed) &&
        renamed, "native Set rename");
    root(destination, "copy", 0);
    require(contents(data, destination, "renamed").size() == 1003,
        "rename payload");
}

static void test_errors_and_replacement() {
    shared_db database = memory();
    dao::paged_set data;
    long long changed;
    require(data.change(database, "set", {"a", "b"}, true, changed), "seed");
    value_record string;
    string.string_value = "wrong";
    std::string bytes;
    require(database->value_codec().encode(string, bytes) ==
        codec_status::ok && database->set("wrong", bytes), "wrong type seed");
    bool moved;
    require(!data.move(database, "set", "wrong", "absent", moved) &&
        data.status() == dao::dao_base::read_status::wrong_type && !moved,
        "SMOVE destination type precedence");
    require(data.move(database, "missing", "wrong", "a", moved) && !moved,
        "SMOVE missing source skips destination type");
    long long count;
    require(!data.intersection_size(database, {"missing", "wrong"}, 1,
        count) && data.status() == dao::dao_base::read_status::wrong_type,
        "SINTERCARD validates all types");
    std::set<std::string> out;
    const std::string target = "wrong";
    require(data.algebra(database, {"set", "set"},
        dao::set_operation::difference, &target, out) && out.empty(),
        "duplicate difference empty");
    root(database, target, 0);
    require(data.algebra(database, {"set"}, dao::set_operation::unite,
        &target, out) && out == std::set<std::string>({"a", "b"}),
        "STORE overwrites destination");
    root(database, target, 2);
}

static void test_rank_selection() {
    shared_db database = memory();
    dao::paged_set data;
    std::vector<std::string> members = {"", std::string("a\0b", 3),
        std::string(40000, 'x')};
    for (unsigned i = 0; i < 300; ++i) members.push_back(std::to_string(i));
    long long changed;
    require(data.change(database, "set", members, true, changed), "rank seed");
    require(database->transact_pages({"set"}, [&](page_transaction& tx) {
        map_store map(tx, "set", map_kind::set);
        if (!map.open()) return false;
        std::vector<std::string> ordered;
        if (!map.visit([&](const std::string& member, const std::string&) {
            ordered.push_back(member); return true;
        }, false)) return false;
        for (size_t rank = 0; rank < ordered.size(); ++rank) {
            std::string member;
            bool found;
            require(map.select(rank, member, found) && found &&
                member == ordered[rank], "subtree rank selection");
        }
        std::string unchanged = "sentinel";
        bool found;
        require(map.select(ordered.size(), unchanged, found) && !found &&
            unchanged == "sentinel", "out-of-range rank");
        return map.finish();
    }) == db::batch_status::ok, "rank traversal");
}

static void test_allocation_rollback() {
    for (unsigned operation = 0; operation < 3; ++operation) {
        bool failed = false, succeeded = false;
        for (long budget = 0; budget < 2000 && !succeeded; ++budget) {
            shared_db database = memory();
            dao::paged_set data;
            long long changed;
            require(data.change(database, "source", {"a", "b", "c"},
                true, changed) && data.change(database, "target", {"old"},
                    true, changed), "fault seed");
            const auto source = contents(data, database, "source");
            const auto target = contents(data, database, "target");
            std::string before_source, before_target;
            require(database->get("source", before_source) &&
                database->get("target", before_target), "fault roots");
            bool ok = false, moved;
            dao::set_sample_result sample;
            std::set<std::string> out;
            const std::string destination = "target";
            allocation_budget = budget;
            try {
                if (operation == 0)
                    ok = data.move(database, "source", "target", "a", moved);
                else if (operation == 1)
                    ok = data.algebra(database, {"source", "target"},
                        dao::set_operation::unite, &destination, out);
                else ok = data.sample(database, "source", 2, true, sample);
            } catch (const std::bad_alloc&) {}
            allocation_budget = -1;
            if (ok) { succeeded = true; continue; }
            failed = true;
            std::string after;
            require(database->get("source", after) && after == before_source
                && database->get("target", after) && after == before_target,
                "failed command changed roots");
            require(contents(data, database, "source") == source &&
                contents(data, database, "target") == target,
                "failed command changed native pages");
            require(out.empty() && sample.values.empty(),
                "failure exposed partial results");
        }
        require(failed && succeeded, "fault sweep incomplete");
    }
}

static void test_mixed_page_reads() {
    shared_db database = memory();
    dao::paged_set data;
    std::vector<std::string> members;
    for (unsigned i = 0; i < 3000; ++i)
        members.push_back("long-member-name:" + std::to_string(i));
    long long count;
    require(data.change(database, "source", members, true, count),
        "mixed seed");
    std::string before, after;
    require(database->get("source", before), "source root");
    require(database->transact_page_reads({"source"},
        [](page_transaction& tx) {
        return tx.put("source", 1, "forbidden");
    }) == db::batch_status::aborted, "read-only pages accepted edit");
    require(database->transact_page_reads({"source"},
        [](page_transaction& tx) {
        return tx.reset_pages("source");
    }) == db::batch_status::aborted, "read-only pages accepted reset");
    require(database->get("source", after) && after == before &&
        contents(data, database, "source").size() == 3000,
        "page-read rejection changed source");
    dao::json_zset sorted;
    std::vector<zset_member> out;
    const std::string target = "source";
    require(sorted.algebra(database, {"source", "source"},
        dao::zset_operation::unite, &target, false,
        [](dao::zset_algebra_options&) { return true; }, out, count) &&
        count == 3000, "mixed Set ZUNIONSTORE alias");
    require(database->get(target, after) && after.size() > 16384,
        "legacy ZSet result was truncated to native root bound");
    std::vector<zset_score> scores;
    require(sorted.scores(database, target, {members[0]}, scores) &&
        scores.size() == 1 && scores[0].found && scores[0].score == 2,
        "mixed Set scores");
    require(database->transact_pages({target}, [&](page_transaction& tx) {
        std::string page;
        return tx.lookup(target, 1, page) == db::lookup_status::missing;
    }) == db::batch_status::ok, "ZSet overwrite retained native Set pages");
}

static void test_failed_migration_and_ttl() {
    shared_db database = memory();
    json_record_codec codec(1);
    value_record old, wrong;
    old.header.kind = value_kind::set;
    old.header.expires_at_ms = deadline;
    old.set_value = {"a", "b", std::string(40000, 'x')};
    std::string before, bad, after;
    require(codec.encode(old, before) == codec_status::ok &&
        codec.encode(wrong, bad) == codec_status::ok &&
        database->set("source", before) && database->set("wrong", bad),
        "migration fixtures");
    dao::paged_set data;
    bool moved;
    require(!data.move(database, "source", "wrong", "a", moved) &&
        data.status() == dao::dao_base::read_status::wrong_type &&
        database->get("source", after) && after == before,
        "failed move partially migrated source");
    require(database->transact_pages({"source"}, [](page_transaction& tx) {
        std::string page;
        return tx.lookup("source", 1, page) == db::lookup_status::missing;
    }) == db::batch_status::ok, "failed migration leaked pages");
    long long count;
    require(data.change(database, "source", {"new"}, true, count),
        "successful migration");
    dao::json_key keys;
    require(keys.expiry_time(database, "source", count) && count == deadline,
        "SADD changed TTL");
    const std::string target = "source";
    std::set<std::string> out;
    require(data.algebra(database, {"source"}, dao::set_operation::unite,
        &target, out) && keys.expiry_time(database, target, count) &&
        count == -1, "STORE alias must clear TTL");
    for (const auto& bytes : {
        std::string("{\"V\":1,\"T\":\"U\",\"E\":-1,\"P\":-1,"
            "\"D\":[\"a\",\"a\"]}"),
        std::string("{\"V\":1,\"T\":\"U\",\"E\":-1,\"P\":-1,"
            "\"D\":[]}"), std::string("PMR\3future", 10)}) {
        require(database->set("bad", bytes) &&
            !data.size(database, "bad", count) &&
            database->get("bad", after) && after == bytes,
            "malformed/future Set was silently replaced");
    }
}

int main() {
    acl::acl_cpp_init();
    test_model();
    test_failed_migration_and_ttl();
    test_mixed_page_reads();
    test_rank_selection();
    test_allocation_rollback();
    test_sampling_and_locality();
    test_migration_scan_copy();
    test_errors_and_replacement();
    std::cout << "Native Set model, sampling and migration passed\n";
}
