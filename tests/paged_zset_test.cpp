#include "stdafx.h"
#include "dao/paged_zset.h"
#include "dao/json/json_zset.h"
#include "dao/json/json_key.h"
#include "dao/json/json_scan.h"
#include "coder/json_record_codec.h"
#include <cmath>
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
static bool use_tbb = false;
static void require(bool ok, const char* reason) {
    if (!ok) { std::cerr << reason << '\n'; std::exit(1); }
}
static shared_db memory() {
    shared_db database;
#if defined(HAS_TBB)
    if (use_tbb) database = db::create_mdb_tbb(4);
#endif
    if (!database) database = db::create_mdb(4);
    require(database->open(""), "memory open");
    return database;
}
static bool same(const std::vector<zset_member>& a,
    const std::vector<zset_member>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i].member != b[i].member || a[i].score != b[i].score ||
            std::signbit(a[i].score) != std::signbit(b[i].score)) return false;
    return true;
}
static std::vector<zset_member> contents(dao::zset& data, shared_db database,
    const std::string& key) {
    std::vector<zset_member> out;
    require(data.range(database, key, 0, -1, false, out), "contents");
    return out;
}
static void root(shared_db database, const std::string& key, size_t size) {
    std::string bytes;
    require(database->get(key, bytes) == (size != 0), "root existence");
    if (!size) return;
    zset_root metadata;
    require(bytes.size() == 64 && zset_page_codec::decode_root(bytes, metadata)
        == map_page_status::ok && metadata.elements == size,
        "whole ZSet stored as one value");
}

static void test_model() {
    auto database = memory(), reference = memory();
    dao::paged_zset pages;
    dao::json_zset json;
    std::mt19937 random(396299);
    const std::vector<std::string> keys = {"a", "b", "c"};
    const std::vector<std::string> members = {"", "x", "y", "z",
        std::string("a\0b", 3), std::string(40000, 'm')};
    for (unsigned step = 0; step < 600; ++step) {
        const auto& key = keys[random() % keys.size()];
        const auto& other = keys[random() % keys.size()];
        const auto& member = members[random() % members.size()];
        const unsigned command = random() % 8;
        long long actual, expected;
        if (command < 2) {
            dao::zadd_options options;
            options.nx = random() % 7 == 0;
            options.xx = !options.nx && random() % 7 == 0;
            options.gt = !options.nx && random() % 5 == 0;
            options.lt = !options.nx && !options.gt && random() % 5 == 0;
            options.incr = random() % 3 == 0;
            const double score = static_cast<int>(random() % 40) - 20;
            dao::zadd_result got, wanted;
            require(pages.add(database, key, {{member, score}}, options, got)
                && json.add(reference, key, {{member, score}}, options, wanted)
                && got.added == wanted.added && got.changed == wanted.changed
                && got.processed == wanted.processed && got.score ==
                    wanted.score, "ZADD model");
        } else if (command == 2) {
            require(pages.remove(database, key, {member, member}, actual) &&
                json.remove(reference, key, {member, member}, expected) &&
                actual == expected, "ZREM model");
        } else if (command == 3) {
            std::vector<zset_member> got, wanted;
            const long long count = random() % 5;
            const bool maximum = random() % 2;
            require(pages.pop(database, key, count, maximum, got) &&
                json.pop(reference, key, count, maximum, wanted) &&
                same(got, wanted), "ZPOP model");
        } else if (command < 7) {
            zset_query query;
            query.mode = static_cast<zrange_mode>(random() % 3);
            query.start = static_cast<int>(random() % 12) - 6;
            query.end = static_cast<int>(random() % 12) - 6;
            query.min_score.value = -10; query.max_score.value = 10;
            query.min_lex.infinity = -1; query.max_lex.infinity = 1;
            query.reverse = random() % 2;
            if (command == 4) {
                require(pages.store_range(database, key, other, query, actual)
                    && json.store_range(reference, key, other, query, expected)
                    && actual == expected, "ZRANGESTORE model");
            } else if (command == 5) {
                require(pages.remove_range(database, key, query, actual) &&
                    json.remove_range(reference, key, query, expected) &&
                    actual == expected, "range removal model");
            } else {
                std::vector<zset_member> got, wanted;
                require(pages.select(database, key, query, got) &&
                    json.select(reference, key, query, wanted) &&
                    same(got, wanted) && pages.count(database, key, query,
                        actual) && actual == static_cast<long long>(got.size()),
                    "range/count model");
            }
        } else {
            dao::zset_algebra_options options;
            options.weights = {2, -1, 3};
            const auto configure = [&](dao::zset_algebra_options& out) {
                out = options; return true;
            };
            const auto operation = static_cast<dao::zset_operation>(
                random() % 3);
            std::vector<zset_member> got, wanted;
            const std::vector<std::string> sources = {key, other, key};
            const std::string* target = random() % 2 ? &key : nullptr;
            require(pages.algebra(database, sources, operation, target, false,
                configure, got, actual) && json.algebra(reference, sources,
                operation, target, false, configure, wanted, expected) &&
                actual == expected && same(got, wanted), "algebra model");
        }
        for (const auto& name : keys) {
            const auto expected = contents(json, reference, name);
            require(same(contents(pages, database, name), expected),
                "full snapshot model");
            root(database, name, expected.size());
        }
    }
}

static void test_migration_and_keys() {
    auto database = memory(), destination = memory();
    dao::paged_zset data;
    value_record record;
    record.header.kind = value_kind::zset;
    record.header.expires_at_ms = deadline;
    record.zset_value = {{"", -0.0}, {"infinite", INFINITY},
        {std::string(1024 * 1024, 'x'), -INFINITY}};
    json_record_codec codec;
    std::string before, after;
    require(codec.encode(record, before) == codec_status::ok &&
        database->set("zset", before), "legacy fixture");
    dao::zadd_options options; options.incr = true;
    dao::zadd_result result;
    require(!data.add(database, "zset", {{"infinite", -INFINITY}}, options,
        result) && data.status() == dao::dao_base::read_status::nan_score &&
        database->get("zset", after) && after == before,
        "failed increment partially migrated ZSet");
    require(database->transact_pages({"zset"}, [](page_transaction& tx) {
        std::string bytes;
        return tx.lookup("zset", 1, bytes) == db::lookup_status::missing;
    }) == db::batch_status::ok, "migration failure leaked pages");
    long long count;
    require(data.size(database, "zset", count) && count == 3,
        "legacy migration");
    root(database, "zset", 3);
    std::vector<zset_score> scores;
    require(data.scores(database, "zset", {""}, scores) && scores[0].found &&
        std::signbit(scores[0].score), "migration lost negative zero");
    dao::json_key keys;
    require(keys.expiry_time(database, "zset", count) && count == deadline,
        "migration TTL");
    bool copied;
    require(keys.copy(database, "zset", destination, "copy", true, copied)
        && copied, "cross-DB native copy");
    bool renamed;
    require(keys.rename(destination, "copy", "renamed", false, renamed) &&
        renamed && keys.expiry_time(destination, "renamed", count) &&
        count == deadline, "native rename/TTL");
    require(data.remove(destination, "renamed", {""}, count) && count == 1 &&
        data.size(database, "zset", count) && count == 3,
        "copy aliases source");
    require(keys.persist(destination, "renamed") &&
        keys.expiry_time(destination, "renamed", count) && count == -1,
        "native persist");
    dao::json_scan scan;
    scan_options request; request.count = 1;
    std::map<std::string, std::string> seen;
    do {
        scan_page page;
        require(scan.open_collection(database, "zset", value_kind::zset) &&
            scan.collection(request, page), "native ZSCAN");
        for (size_t i = 0; i < page.values.size(); i += 2)
            seen.emplace(page.values[i], page.values[i + 1]);
        request.cursor = page.cursor;
    } while (request.cursor);
    require(seen.size() == 3 && seen.at("") == "-0",
        "scan lost members/zero");
}

static void test_expired_page_reclamation() {
    auto database = memory();
    dao::paged_zset data;
    dao::zadd_result out;
    require(data.add(database, "zset",
        {{std::string(100000, 'x'), 1}}, {}, out),
        "large native fixture");
    zset_root previous;
    require(database->transact_pages({"zset"}, [&](page_transaction& tx) {
        auto& entry = *tx.roots().find("zset");
        if (zset_page_codec::decode_root(entry.value, previous) !=
            map_page_status::ok) return false;
        auto expired = previous; expired.expires_at_ms = 1;
        entry.action = db::update_action::write;
        return zset_page_codec::encode_root(expired, entry.value) ==
            map_page_status::ok;
    }) == db::batch_status::ok, "stale expiry root");
    require(data.add(database, "zset", {{"new", 4}}, {}, out) && out.added == 1,
        "expired native replacement");
    require(database->transact_pages({"zset"}, [&](page_transaction& tx) {
        zset_root root;
        if (zset_page_codec::decode_root(tx.roots().find("zset")->value, root)
            != map_page_status::ok || root.elements != 1 || root.last_id != 2
            || root.expires_at_ms != -1) return false;
        for (uint64_t id = 3; id <= previous.last_id; ++id) {
            std::string bytes;
            if (tx.lookup("zset", id, bytes) != db::lookup_status::missing)
                return false;
        }
        return true;
    }) == db::batch_status::ok, "expired key leaked old member pages");
}

static void test_failure_rollback() {
    bool failed = false, success = false;
    for (long budget = 0; budget < 2000 && !success; ++budget) {
        auto database = memory();
        dao::paged_zset data;
        dao::zadd_result out;
        require(data.add(database, "zset", {{"a", 1}, {"b", 2}}, {}, out),
            "fault seed");
        const auto initial = contents(data, database, "zset");
        std::string before, after;
        require(database->get("zset", before), "fault root");
        const std::vector<zset_member> changes = {{"a", 7},
            {std::string(40000, 'x'), 3}};
        bool ok = false;
        allocation_budget = budget;
        try { ok = data.add(database, "zset", changes, {}, out); }
        catch (const std::bad_alloc&) {}
        allocation_budget = -1;
        if (ok) { success = true; continue; }
        failed = true;
        require(database->get("zset", after) && before == after &&
            same(contents(data, database, "zset"), initial),
            "failed ZADD changed native indexes");
    }
    require(failed && success, "incomplete ZADD allocation sweep");
}

static void test_complex_failures() {
    for (unsigned command = 0; command < 3; ++command) {
        bool failed = false, success = false;
        for (long budget = 0; budget < 2000 && !success; ++budget) {
            auto database = memory();
            dao::paged_zset data;
            dao::zadd_result added;
            require(data.add(database, "a", {{"x", 1}, {"y", 2}}, {}, added)
                && data.add(database, "b", {{"z", 3}}, {}, added),
                "fault seed");
            const auto a = contents(data, database, "a");
            const auto b = contents(data, database, "b");
            std::string root_a, root_b, after;
            require(database->get("a", root_a) && database->get("b", root_b),
                "fault roots");
            const std::vector<std::string> keys = {"a", "b"};
            const std::string target = "a";
            const auto configure = [](dao::zset_algebra_options&) {
                return true;
            };
            std::vector<zset_member> values;
            dao::zset_pop_result popped;
            std::string selected;
            long long count = 0;
            bool ok = false;
            allocation_budget = budget;
            try {
                if (command == 0) ok = data.algebra(database, keys,
                    dao::zset_operation::unite, &target, false, configure,
                    values, count);
                else if (command == 1) ok = data.store_range(database, "a",
                    "b", zset_query(), count);
                else ok = data.mpop(database, keys, true, 1, selected, popped);
            } catch (const std::bad_alloc&) {}
            allocation_budget = -1;
            if (ok) { success = true; continue; }
            failed = true;
            require(database->get("a", after) && after == root_a &&
                database->get("b", after) && after == root_b &&
                same(contents(data, database, "a"), a) &&
                same(contents(data, database, "b"), b),
                "failed complex command changed roots/pages");
            require(values.empty() && popped.values.empty() && selected.empty(),
                "failed complex command exposed partial results");
        }
        require(failed && success, "incomplete complex allocation sweep");
    }
}

static void test_invalid_records_and_sampling() {
    auto database = memory();
    dao::paged_zset data;
    long long count;
    for (const auto& bytes : {
        std::string("{\"V\":1,\"T\":\"Z\",\"E\":-1,\"P\":-1,"
            "\"D\":[[\"a\",\"1\"],[\"a\",\"2\"]]}"),
        std::string("{\"V\":1,\"T\":\"Z\",\"E\":-1,\"P\":-1,"
            "\"D\":[]}"), std::string("PZR\2future", 10)}) {
        std::string after;
        require(database->set("bad", bytes) &&
            !data.size(database, "bad", count) && database->get("bad", after)
            && after == bytes, "invalid/future ZSet was overwritten");
    }
    dao::zadd_result added;
    require(data.add(database, "zset", {{"a", -0.0}, {"b", 2}, {"c", 3}},
        {}, added), "sample seed");
    dao::zset_sample_result sample;
    require(data.sample(database, "zset", 2, sample) && sample.found &&
        sample.values.size() == 2 && sample.values[0].member !=
            sample.values[1].member, "unique sample");
    require(data.sample(database, "zset", -40, sample) && sample.found &&
        sample.values.size() == 40, "repeated sample");
    for (const auto& member : sample.values)
        require(member.member == "a" || member.member == "b" ||
            member.member == "c", "sample invented member");
    require(!data.sample(database, "zset", LLONG_MIN, sample) &&
        sample.values.empty() && !sample.found, "invalid sample count");
}

int main() {
    acl::acl_cpp_init();
    test_model(); test_migration_and_keys(); test_expired_page_reclamation();
    test_failure_rollback(); test_complex_failures();
    test_invalid_records_and_sampling();
#if defined(HAS_TBB)
    use_tbb = true;
    test_model(); test_migration_and_keys(); test_expired_page_reclamation();
    test_failure_rollback(); test_complex_failures();
    test_invalid_records_and_sampling();
#endif
    std::cout << "Native ZSet DAO, migration and key integration passed\n";
}
