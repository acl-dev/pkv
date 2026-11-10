#include "stdafx.h"
#include "coder/compact_zset.h"
#include "dao/paged_set.h"
#include "dao/paged_zset.h"
#include "dao/json/json_zset.h"
#include "dao/json/json_key.h"
#include "dao/json/json_scan.h"
#include "coder/json_record_codec.h"
#include <cmath>
#include <cstdio>
#include <limits>
#include <cstdlib>
#include <iostream>
#include <new>
#include <random>
#include <thread>

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
    if (compact_zset::matches(bytes)) {
        const compact_zset::view packed(bytes);
        packed.validate_fast();
        require(packed.count() == size, "compact ZSet root cardinality");
        require(database->transact_pages({key}, [&](page_transaction& tx) {
            std::string page;
            return tx.lookup(key, 1, page) == db::lookup_status::missing;
        }) == db::batch_status::ok, "compact ZSet owns pages");
        return;
    }
    zset_root metadata;
    require(zset_page_codec::decode_root(bytes, metadata)
        == map_page_status::ok && metadata.elements == size,
        "whole ZSet stored as one value");
}

static void test_bulk_add_new_key() {
    auto database = memory(), incremental = memory();
    dao::paged_zset data;
    std::vector<zset_member> input;
    for (unsigned i = 0; i < 160; ++i) {
        auto member = std::to_string(i) + std::string(i % 3 * 100, '\0');
        input.push_back({std::move(member), static_cast<double>(i)});
    }
    input.push_back({input[0].member, -0.0});
    input.push_back({input[1].member, 400});
    input.push_back({input[1].member, 400});
    input.push_back({input[1].member, -2});
    dao::zadd_options options;
    options.ch = true;
    dao::zadd_result result;
    require(data.add(database, "bulk", input, options, result) &&
        result.added == 160 && result.changed == 162 && result.processed &&
        result.score == -2, "bulk ZADD duplicate accounting");
    for (const auto& item : input)
        require(data.add(incremental, "bulk", {item}, options, result),
            "incremental ZADD model");
    require(same(contents(data, database, "bulk"),
        contents(data, incremental, "bulk")), "bulk ZADD rank and score model");
    root(database, "bulk", 160);
}

static void test_bulk_store_alias() {
    auto database = memory();
    dao::paged_zset data;
    dao::json_key keys;
    const std::string target = "alias";
    const std::string other = "other";
    std::vector<zset_member> left, right, expected;
    for (unsigned i = 0; i < 288; ++i) {
        const auto name = std::to_string(i) + std::string(i % 2 ? 80 : 140, '\0');
        if (i < 192) left.push_back({name, double(i)});
        if (i >= 96) right.push_back({name, 2.0 * i});
        expected.push_back({name, i < 96 ? 2.0 * i : i < 192 ? 8.0 * i : 6.0 * i});
    }
    dao::zadd_result added;
    require(data.add(database, target, left, {}, added) &&
        data.add(database, other, right, {}, added) &&
        keys.set_expiry(database, target, deadline, 0) &&
        keys.set_expiry(database, other, deadline, 0), "bulk store fixtures");
    std::vector<watch_token> watch;
    require(database->watch({target, other}, watch) == db::batch_status::ok,
        "bulk store WATCH");
    const auto configure = [](dao::zset_algebra_options& options) {
        options.weights = {2, 3};
        return true;
    };
    std::vector<zset_member> ignored;
    long long stored, expiry;
    std::sort(expected.begin(), expected.end(), zset_less);
    require(data.algebra(database, {target, other}, dao::zset_operation::unite,
        &target, false, configure, ignored, stored) && stored == 288 &&
        same(contents(data, database, target), expected) &&
        same(contents(data, database, other), right), "bulk union store alias");
    require(!watch[0]->valid(0) && watch[1]->valid(0) &&
        keys.expiry_time(database, target, expiry) && expiry == -1 &&
        keys.expiry_time(database, other, expiry) && expiry == deadline,
        "bulk union store expiry and WATCH");
    const auto inline_limit = database->supports_compact_zsets() ?
        map_page_codec::zset_key_inline_bytes : map_page_codec::key_inline_bytes;
    uint64_t pages = 2 * expected.size();
    for (const auto& value : expected) {
        pages += value.member.size() > inline_limit;
        pages += value.member.size() + 8 > inline_limit &&
            (!database->supports_compact_zsets() || value.member.size() <= inline_limit);
    }
    zset_root meta;
    std::string encoded;
    require(database->get(target, encoded) &&
        zset_page_codec::decode_root(encoded, meta) == map_page_status::ok &&
        meta.last_id == pages, "bulk store should inline medium members");

    zset_query query;
    query.start = 20;
    query.end = 179;
    query.reverse = true;
    const std::vector<zset_member> selected(expected.end() - 180, expected.end() - 20);
    require(keys.set_expiry(database, target, deadline, 0) &&
        data.store_range(database, target, target, query, stored) && stored == 160 &&
        same(contents(data, database, target), selected) &&
        keys.expiry_time(database, target, expiry) && expiry == -1,
        "bulk reverse range store alias");
}

static void test_batched_intersection_cardinality() {
    auto database = memory();
    dao::paged_zset zsets;
    dao::paged_set sets;
    std::vector<zset_member> left, right;
    std::vector<std::string> members;
    for (unsigned i = 0; i < 256; ++i) {
        auto member = std::to_string(i) + std::string(i % 13 == 0 ? 150 : 16, '\0');
        if (i < 192) left.push_back({member, double(i)});
        if (i >= 32 && i < 225) right.push_back({member, double(i) + .25});
        if (i >= 64) members.push_back(std::move(member));
    }
    dao::zadd_result added;
    long long count;
    require(zsets.add(database, "left", left, {}, added) &&
        zsets.add(database, "right", right, {}, added) &&
        sets.change(database, "set", members, true, count), "batched intersection fixtures");
    std::vector<zset_member> out;
    const auto verify = [&](const std::vector<std::string>& keys, long long limit,
        long long expected) {
        const auto configure = [&](dao::zset_algebra_options& options) {
            options.limit = limit;
            return true;
        };
        require(zsets.algebra(database, keys, dao::zset_operation::intersect,
            nullptr, true, configure, out, count) && count == expected && out.empty(),
            "batched intersection result");
    };
    for (long long limit : {0LL, 1LL, 15LL, 16LL, 31LL, 32LL, 33LL, 127LL,
            128LL, 129LL, LLONG_MAX}) {
        verify({"left", "right"}, limit, limit ? std::min(limit, 160LL) : 160);
        verify({"set", "right", "left", "right"}, limit,
            limit ? std::min(limit, 128LL) : 128);
    }
    verify({"left", "right", "missing"}, 32, 0);

    // Keep the root cardinality intact so the corruption is found while
    // checking the indexes of the lead or the secondary collection.
    const auto corrupt = [&](const std::string& key) {
        require(database->transact_pages({key}, [&](page_transaction& tx) {
            zset_root root;
            std::string bytes;
            map_node node;
            if (zset_page_codec::decode_root(tx.roots().find(key)->value, root) !=
                    map_page_status::ok ||
                tx.lookup(key, root.scores, bytes) != db::lookup_status::found ||
                map_page_codec::decode_node(bytes, node) != map_page_status::ok) return false;
            node.value.length = 1;
            node.value.inline_value = "x";
            return map_page_codec::encode_node(node, bytes) == map_page_status::ok &&
                tx.put(key, root.scores, bytes);
        }) == db::batch_status::ok, "batched intersection corrupt fixture");
    };
    const auto configure = [](dao::zset_algebra_options&) { return true; };
    const auto rejected = [&](const std::string& secondary) {
        require(!zsets.algebra(database, {"left", secondary}, dao::zset_operation::intersect,
            nullptr, true, configure, out, count) && count == 0 && out.empty() &&
            zsets.status() == dao::dao_base::read_status::invalid_record,
            "batched intersection accepted a corrupt score index");
    };
    corrupt("right");
    rejected("right");
    require(zsets.add(database, "other", right, {}, added), "batched intersection alternate source");
    corrupt("left");
    rejected("other");
}

static void test_native_score_moves() {
    auto database = memory(), reference = memory();
    dao::paged_zset pages;
    dao::json_zset json;
    std::vector<zset_member> seed;
    for (unsigned i = 0; i < 129; ++i)
        seed.push_back({"member:" + std::to_string(1000000 + i), double(i)});
    dao::zadd_result actual, expected;
    require(pages.add(database, "native", seed, {}, actual) &&
        json.add(reference, "native", seed, {}, expected), "native move seed");
    std::mt19937 random(659171);
    for (unsigned step = 0; step < 4000; ++step) {
        const auto index = random() % seed.size();
        const auto& member = seed[index].member;
        dao::zadd_options options;
        options.ch = true;
        options.incr = step % 4 == 0;
        double score = (step % 2 ? -1 : 1) / 256.0;
        if (!options.incr) {
            switch (step % 8) {
            case 1: score = -0.0; break;
            case 2: score = std::numeric_limits<double>::infinity(); break;
            case 3: score = -std::numeric_limits<double>::infinity(); break;
            default: score = static_cast<int>(random() % 1024) - 512;
            }
        }
        require(pages.add(database, "native", {{member, score}}, options, actual) &&
            json.add(reference, "native", {{member, score}}, options, expected) &&
            actual.added == expected.added && actual.changed == expected.changed &&
            actual.score == expected.score, "native score move result");
        if (step % 17 == 0) {
            const auto ordered = contents(pages, database, "native");
            require(same(ordered, contents(json, reference, "native")),
                "native score move order");
            const auto rank = random() % ordered.size();
            zset_rank got;
            require(pages.rank(database, "native", ordered[rank].member, false,
                got) && got.found && got.rank == static_cast<long long>(rank),
                "native score move rank");
        }
    }
    require(same(contents(pages, database, "native"),
        contents(json, reference, "native")), "native final score order");
    root(database, "native", 129);
}

static void test_native_score_move_failure() {
    bool failed = false, success = false;
    for (long budget = 0; budget < 2000 && !success; ++budget) {
        auto database = memory();
        dao::paged_zset data;
        std::vector<zset_member> seed;
        for (unsigned i = 0; i < 129; ++i)
            seed.push_back({"member:" + std::to_string(1000000 + i), double(i)});
        dao::zadd_result result;
        require(data.add(database, "native", seed, {}, result), "native fault seed");
        const auto before = contents(data, database, "native");
        std::string root_before, root_after;
        require(database->get("native", root_before), "native fault root");
        const std::vector<zset_member> change{{seed.front().member, -.125}};
        bool ok = false;
        allocation_budget = budget;
        try { ok = data.add(database, "native", change, {}, result); }
        catch (const std::bad_alloc&) {}
        allocation_budget = -1;
        if (ok) {
            success = true;
            auto after = contents(data, database, "native");
            require(after.size() == 129 && after.front().score == -.125,
                "native fault sweep success lost update");
        } else {
            failed = true;
            require(database->get("native", root_after) && root_after == root_before &&
                same(contents(data, database, "native"), before),
                "failed native score replacement changed indexes");
        }
    }
    require(failed && success, "incomplete native score failure sweep");
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
    std::vector<zset_member> values;
    long long cardinality;
    const auto configure = [](dao::zset_algebra_options&) { return true; };
    require(data.algebra(database, {"zset"}, dao::zset_operation::intersect,
        nullptr, true, configure, values, cardinality) && cardinality == 0,
        "algebra exposed expired native source");
    require(database->transact_pages({"zset"}, [](page_transaction& tx) {
        const auto& entry = *tx.roots().find("zset");
        zset_root expired;
        return entry.found && zset_page_codec::decode_root(entry.value,
            expired) == map_page_status::ok && expired.expires_at_ms == 1;
    }) == db::batch_status::ok, "algebra erased expired source");
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
            "\"D\":[]}"), std::string("PZR\177future", 10)}) {
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

static void test_compact_validation_cache() {
    using namespace compact_zset;
    std::vector<entry> values;
    for (unsigned i = 0; i < 128; ++i)
        values.emplace_back(std::string(1, static_cast<char>(i)) + std::string(19, 'x'), .1);
    auto bytes = encode(values, deadline);
    require(!view(bytes).validate_fast(false) &&
        validation_cache::retained_capacity() <= std::string().capacity(),
        "nonretaining validation allocated a cache buffer");
    require(!view(bytes).validate_fast() && view(bytes).validate_fast(),
        "extended root validation cache miss/hit");
    require(validation_cache::retained_capacity() >= bytes.size() &&
        validation_cache::retained_capacity() < validation_cache::max_bytes,
        "cache must grow to actual root size, not maximum root size");
    const auto copy = bytes;
    require(view(copy).validate_fast(), "cache must compare bytes across different addresses");
    {
        const view same_address(bytes);
        const auto ranks = size_t(14 + 2 * 128);
        const auto saved = bytes[ranks + 127];
        bytes[ranks + 127] = bytes[ranks];
        bool rejected = false;
        try { same_address.validate_fast(); }
        catch (const std::runtime_error&) { rejected = true; }
        require(rejected, "same-address malformed mutation bypassed validation");
        bytes[ranks + 127] = saved;
        require(same_address.validate_fast(), "failed validation poisoned previous valid cache");
    }
    view(bytes).replace_deadline(bytes, 0);
    require(!view(bytes).validate_fast() && view(bytes).deadline() == 0,
        "changed TTL reused stale validation");
    view(bytes).replace_deadline(bytes, deadline);
    require(!view(bytes).validate_fast(), "restored TTL must compare the complete root");
    {
        validation_cache busy;
        require(!view(bytes).validate_fast(), "reentrant cache must fall back to validation");
    }
    std::thread cold([&] {
        allocation_budget = 0;
        const bool miss = !view(bytes).validate_fast();
        allocation_budget = -1;
        require(miss && validation_cache::retained_capacity() <= std::string().capacity(),
            "optional cache allocation failure affected validation or retained capacity");
        require(!view(bytes).validate_fast() && view(bytes).validate_fast(),
            "thread-local cache did not recover after allocation failure");
    });
    cold.join();
    values.clear();
    for (unsigned i = 0; i < 128; ++i)
        values.emplace_back(std::string(1, static_cast<char>(i)) + std::string(94, 'x'), 0);
    const auto largest = encode(values, deadline);
    require(largest.size() == validation_cache::max_bytes &&
        !view(largest).validate_fast() && view(largest).validate_fast() &&
        validation_cache::retained_capacity() <= validation_cache::max_capacity,
        "maximum root cache capacity or round trip");
    require(!view(bytes).validate_fast() && view(bytes).validate_fast() &&
        validation_cache::retained_capacity() <= validation_cache::max_capacity,
        "small root after maximum retained an unbounded cache");
    std::string retained = bytes;
    for (unsigned i = 0; i < validation_miss_limit; ++i) {
        view(bytes).replace_deadline(bytes, deadline + i + 1);
        require(!view(bytes).validate_fast(), "distinct root must miss validation cache");
        if (!validation_access().cooldown) retained = bytes;
    }
    require(validation_access().cooldown == validation_cooldown,
        "repeated misses must enter bounded validation-only cooldown");
    allocation_budget = 0;
    bool validated = true;
    while (validation_access().cooldown) {
        const bool miss = !view(retained).validate_fast();
        validated = validated && miss;
    }
    allocation_budget = -1;
    require(validated && view(retained).validate_fast(),
        "cooldown must validate without allocating and recover cache hits");
}

static void test_compact_single_erase() {
    using namespace compact_zset;
    const double inf = std::numeric_limits<double>::infinity();
    const std::array<double, 8> scores{{0, 63, -64, 1.25, -0.0, .1, -inf, inf}};
    for (unsigned count : {2U, 16U, 32U, 63U, 64U, 128U}) {
        for (size_t width : {size_t(3), size_t(9), size_t(31), size_t(32)}) {
            for (int64_t expiry : {int64_t(-1), int64_t(deadline)}) {
                std::vector<entry> values;
                for (unsigned i = 0; i < count; ++i) {
                    std::string member(width, '\0');
                    member[0] = static_cast<char>(i);
                    member.back() = static_cast<char>(255 - i);
                    values.emplace_back(std::move(member), scores[(i * 5) % scores.size()]);
                }
                const auto initial = encode(values, expiry);
                const view original(initial);
                original.validate_fast();
                for (unsigned rank = 0; rank < count; ++rank) {
                    const auto index = original.ordered_index(rank);
                    erase_mask erased;
                    erased.set(index);
                    const auto expected = original.without(erased, count - 1);
                    auto edited = initial;
                    view(edited).erase_one(edited, index, rank);
                    require(edited == expected, "compact single erase differs from rebuild");
                    const view after(edited);
                    after.validate_fast();
                    require(after.deadline() == expiry, "compact single erase lost TTL");
                }
            }
        }
    }
}

static void test_extended_compact_codec() {
    using namespace compact_zset;
    for (unsigned count : {63U, 64U, 127U, 128U}) {
        std::vector<entry> values;
        for (unsigned i = 0; i < count; ++i) {
            char member[8];
            std::snprintf(member, sizeof member, "%03u", i);
            values.emplace_back(member, 0);
        }
        auto bytes = encode(values, deadline);
        require(byte(bytes[3]) == (count <= 63 ? 3U : 5U), "compact version boundary");
        {
            const view packed(bytes);
            packed.validate_fast();
            require(packed.count() == count && packed.deadline() == deadline &&
                packed.entries() == values, "extended compact round trip");
        }
        for (int64_t expiry : {int64_t(-1), int64_t(0), int64_t(deadline)}) {
            view(bytes).replace_deadline(bytes, expiry);
            const view packed(bytes);
            packed.validate_fast();
            require(packed.count() == count && packed.deadline() == expiry &&
                packed.entries() == values, "extended count confused with TTL flag");
        }
        erase_mask erased;
        for (unsigned index : {0U, 63U, 64U, 127U}) if (index < count) erased.set(index);
        std::vector<entry> kept;
        for (unsigned i = 0; i < count; ++i) if (!erased[i]) kept.push_back(values[i]);
        bytes = view(bytes).without(erased, static_cast<unsigned>(kept.size()));
        require(bytes == encode(kept, deadline), "extended erase/remap or legacy downgrade");
        for (unsigned i = 0; i < count; ++i) if (erased[i]) {
            const view packed(bytes);
            packed.validate_fast();
            unsigned insertion;
            require(packed.member_index(values[i].first, nullptr, &insertion) == packed.count() &&
                packed.insert_member(values[i].first, 0, insertion, bytes), "extended reinsert");
        }
        require(bytes == encode(values, deadline), "extended growth across 63/64 boundary");
        if (count == 128) {
            auto rejects = [&](const std::string& bad) {
                bool failed = false;
                try { view(bad).validate_fast(); } catch (const std::runtime_error&) { failed = true; }
                require(failed, "accepted malformed extended compact root");
            };
            auto bad = bytes; bad[5] = static_cast<char>(129); rejects(bad);
            bad = bytes; bad[5] = 0; rejects(bad);
            bad = bytes; bad[4] |= 1; rejects(bad);
            const auto ranks = size_t(14 + 2 * count);
            bad = bytes; write_offset(&bad[16], 2, 0); rejects(bad);
            bad = bytes; write_offset(&bad[16], 2, 65535); rejects(bad);
            bad = bytes; bad[ranks + count] = static_cast<char>(double_tag); rejects(bad);
            bad = bytes; bad[ranks + count] = static_cast<char>(255); rejects(bad);
            bad = bytes; bad[ranks] = static_cast<char>(128); rejects(bad);
            const auto rank = view(bytes).rank_of(127);
            bad = bytes; bad[ranks + (rank + 1) % count] = 127; rejects(bad);
            bad = bytes; bad.pop_back(); rejects(bad);
        }
    }
    std::vector<entry> values;
    for (unsigned i = 0; i < 8; ++i) values.emplace_back(std::string(255, 'a' + i), 0);
    const auto legacy = encode(values, deadline);
    require(byte(legacy[3]) == 3, "legacy 2048 payload encoding changed");
    values.emplace_back("", 0);
    auto extended = encode(values, deadline);
    require(byte(extended[3]) == 5, "2049 payload should use extended version");
    view(extended).validate_fast();
    auto invalid_legacy = extended;
    invalid_legacy.erase(5, 1);
    invalid_legacy[3] = 3;
    invalid_legacy[4] = static_cast<char>((byte(invalid_legacy[4]) & ~63U) | values.size());
    bool rejected = false;
    try { view(invalid_legacy).validate_fast(); }
    catch (const std::runtime_error&) { rejected = true; }
    require(rejected, "extended payload weakened legacy decoder limit");
    erase_mask erased; erased.set(0); // empty member sorts first
    require(view(extended).without(erased, 8) == legacy, "payload downgrade lost legacy bytes");
    const double infinity = std::numeric_limits<double>::infinity();
    const std::array<double, 10> scores = {{-infinity, -32768, -128,
        -1.0 / 256, -0.0, 0.0, 1.0 / 256, 32767, .1, infinity}};
    std::vector<entry> binary;
    for (unsigned i = 0; i < 128; ++i)
        binary.emplace_back(std::string(1, static_cast<char>(i)) +
            std::string("\0\xff", 2), scores[i % scores.size()]);
    const auto encoded = encode(binary, deadline);
    const auto decoded = view(encoded).entries();
    require(decoded.size() == binary.size(), "extended binary score count");
    for (size_t i = 0; i < binary.size(); ++i)
        require(decoded[i].first == binary[i].first && decoded[i].second == binary[i].second &&
            std::signbit(decoded[i].second) == std::signbit(binary[i].second),
            "extended binary member or exact score lost");
    auto bad_score = encoded;
    bad_score[view(encoded).at(127).second] = static_cast<char>(255);
    rejected = false;
    try { view(bad_score).validate_fast(); }
    catch (const std::runtime_error&) { rejected = true; }
    require(rejected, "extended invalid score tag accepted");
    std::vector<entry> redis_limit;
    for (unsigned i = 0; i < 128; ++i) {
        std::string member(64, static_cast<char>(i));
        redis_limit.emplace_back(std::move(member), .1);
    }
    const auto wide = encode(redis_limit, deadline);
    const view wide_view(wide);
    wide_view.validate_fast();
    require(byte(wide[3]) == 5 && wide_view.count() == 128 &&
        wide_view.deadline() == deadline && wide_view.entries() == redis_limit &&
        wide.size() <= map_page_codec::max_page_bytes,
        "Redis default compact limit with full double scores must fit one root");
}

static std::vector<zset_member> extended_members(unsigned count) {
    std::vector<zset_member> values;
    for (unsigned i = 0; i < count; ++i) {
        char member[32];
        std::snprintf(member, sizeof member, "member:%013u", i);
        values.push_back({member, static_cast<double>(i % 9) - 4});
    }
    return values;
}

static void test_compact_score_moves() {
    using namespace compact_zset;
    std::mt19937_64 random(76);
    const double infinity = std::numeric_limits<double>::infinity();
    const double extremes[] = {-infinity, -1e300, -32768, -.1, -0.0,
        0.0, .1, 32767, 1e300, infinity};
    for (unsigned count : {1U, 2U, 7U, 8U, 63U, 64U, 127U, 128U}) {
        std::vector<entry> values;
        for (const auto& member : extended_members(count))
            values.emplace_back(member.member, 0);
        auto bytes = encode(values, deadline);
        for (unsigned step = 0; step < 500; ++step) {
            const unsigned index = random() % count;
            const double next = step % 3 ? extremes[random() % 10] :
                static_cast<double>(static_cast<int>(random() % 257) - 128) / 8;
            const view packed(bytes);
            packed.validate_fast();
            require(packed.replace_score(bytes, index, next), "score move must fit");
            values[index].second = next;
            const view changed(bytes);
            const auto actual = changed.entries(); // validates member and score order
            require(changed.deadline() == deadline && actual.size() == values.size(),
                "score move lost TTL or members");
            for (unsigned i = 0; i < count; ++i)
                require(actual[i].first == values[i].first &&
                    actual[i].second == values[i].second &&
                    std::signbit(actual[i].second) == std::signbit(values[i].second),
                    "score move differs from model");
        }
    }
}

static void test_compact_rank_permutations() {
    using namespace compact_zset;
    const double inf = std::numeric_limits<double>::infinity();
    const double extremes[] = {-inf, -1e300, -0.0, 0.0, .1, 1.0 / 256, inf};
    for (unsigned count : {1U, 2U, 8U, 16U, 32U, 63U, 64U, 128U}) {
        for (unsigned pattern = 0; pattern < 3; ++pattern) {
            std::vector<entry> values;
            for (const auto& member : extended_members(count)) {
                const unsigned i = static_cast<unsigned>(values.size());
                const double score = !pattern ? 0 : pattern == 1 ?
                    (i % 3 ? double(i) : -double(i)) : extremes[i % 7];
                values.emplace_back(member.member, score);
            }
            const auto bytes = encode(values, deadline);
            view(bytes).validate();
            const auto width = byte(bytes[4]) & wide_offsets_flag ? 2U : 1U;
            const auto ranks = (byte(bytes[3]) == 5 ? 14U : 13U) + width * count;
            for (unsigned position = 0; position < count; ++position) {
                for (unsigned replacement = 0; replacement <= count; ++replacement) {
                    if (replacement == byte(bytes[ranks + position])) continue;
                    auto bad = bytes;
                    bad[ranks + position] = static_cast<char>(replacement);
                    bool rejected = false;
                    try { view(bad).validate(); }
                    catch (const std::runtime_error&) { rejected = true; }
                    require(rejected, "duplicate or out-of-bounds rank accepted");
                }
            }
        }
    }
}

static void test_extended_compact_model() {
    auto database = memory(), reference = memory();
    dao::paged_zset data;
    dao::json_zset json;
    const auto values = extended_members(128);
    dao::zadd_result got, expected;
    require(data.add(database, "z", values, {}, got) &&
        json.add(reference, "z", values, {}, expected), "extended model seed");
    dao::json_key keys;
    // TTL's generic legacy migration would replace the JSON oracle with a
    // native tree; only the candidate needs the deadline under test.
    require(keys.set_expiry(database, "z", deadline, 0), "extended model TTL");
    auto verify = [&]() {
        const auto actual = contents(data, database, "z");
        require(same(actual, contents(json, reference, "z")), "extended score/rank model");
        root(database, "z", actual.size());
        std::string bytes;
        require(database->get("z", bytes) &&
            compact_zset::matches(bytes) == (actual.size() <= 128),
            "extended bounded model unexpectedly changed storage kind");
        long long expiry;
        require(keys.expiry_time(database, "z", expiry) && expiry == deadline,
            "extended operation lost TTL");
        for (size_t i : {size_t(0), actual.size() / 2, actual.size() - 1}) {
            zset_rank rank;
            require(data.rank(database, "z", actual[i].member, false, rank) &&
                rank.found && rank.rank == static_cast<long long>(i) &&
                data.rank(database, "z", actual[i].member, true, rank) &&
                rank.found && rank.rank == static_cast<long long>(actual.size() - i - 1),
                "extended forward/reverse rank");
        }
    };
    verify();
    for (unsigned step = 0; step < 128; ++step) {
        const auto& member = values[(step * 37) % 128].member;
        const double score = static_cast<int>((step * 17) % 101) - 50 +
            (step % 3) * .125;
        require(data.add_one(database, "z", member, score, {}, got) &&
            json.add(reference, "z", {{member, score}}, {}, expected) &&
            got.added == expected.added && got.changed == expected.changed,
            "extended score mutation");
        if (!(step % 16)) verify();
    }
    std::vector<std::string> removed = {values[63].member, values[64].member,
        values[127].member, values[127].member};
    long long actual_count, expected_count;
    require(data.remove(database, "z", removed, actual_count) &&
        json.remove(reference, "z", removed, expected_count) &&
        actual_count == expected_count && actual_count == 3, "extended high ZREM");
    verify();
    require(data.add(database, "z", values, {}, got) &&
        json.add(reference, "z", values, {}, expected), "extended refill");
    zset_query query;
    query.start = 63; query.end = 127;
    require(data.remove_range(database, "z", query, actual_count) &&
        json.remove_range(reference, "z", query, expected_count) &&
        actual_count == 65 && actual_count == expected_count, "extended rank-range removal");
    verify();
    require(data.add(database, "z", values, {}, got) &&
        json.add(reference, "z", values, {}, expected), "extended pop refill");
    std::vector<zset_member> actual_pop, expected_pop;
    for (long long count : {64LL, 1LL}) {
        require(data.pop(database, "z", count, true, actual_pop) &&
            json.pop(reference, "z", count, true, expected_pop) &&
            same(actual_pop, expected_pop), "extended maximum pop");
        verify();
    }
    require(data.add(database, "z", values, {}, got) &&
        json.add(reference, "z", values, {}, expected) &&
        data.add_one(database, "z", "overflow", 1, {}, got) &&
        json.add(reference, "z", {{"overflow", 1}}, {}, expected), "extended promotion");
    verify();
}

static void test_extended_compact_failures() {
    for (unsigned operation = 0; operation < 3; ++operation) {
        bool failed = false, succeeded = false;
        auto database = memory();
        dao::paged_zset data;
        dao::zadd_result added;
        const auto values = extended_members(operation ? 128 : 63);
        require(data.add(database, "z", values, {}, added), "extended fault seed");
        dao::json_key keys;
        require(keys.set_expiry(database, "z", deadline, 0), "extended fault TTL");
        std::string before;
        require(database->get("z", before) && compact_zset::matches(before),
            "extended fault root");
        const auto previous = contents(data, database, "z");
        for (long budget = 0; budget < 4096 && !succeeded; ++budget) {
            bool ok = false;
            long long removed;
            allocation_budget = budget;
            try {
                if (operation == 0)
                    ok = data.add_one(database, "z", "next", 0, {}, added);
                else if (operation == 1)
                    ok = data.remove(database, "z", {values[63].member,
                        values[64].member, values[127].member}, removed);
                else ok = data.add_one(database, "z", "overflow", 0, {}, added);
            } catch (const std::bad_alloc&) {}
            allocation_budget = -1;
            if (ok) { succeeded = true; continue; }
            failed = true;
            std::string after;
            require(database->get("z", after) && after == before &&
                same(contents(data, database, "z"), previous),
                "extended failure changed roots or values");
            root(database, "z", values.size());
            require(database->transact_page_reads({"z"}, [&](page_transaction& tx) {
                for (uint64_t id = 1; id <= 260; ++id) {
                    std::string page;
                    if (tx.lookup("z", id, page) != db::lookup_status::missing)
                        return false;
                }
                return true;
            }) == db::batch_status::ok, "failed compact promotion leaked native pages");
        }
        require(failed && succeeded, "extended fault sweep incomplete");
        root(database, "z", operation == 0 ? 64 : operation == 1 ? 125 : 129);
        auto expected = values;
        if (operation == 1) {
            expected.erase(expected.begin() + 127);
            expected.erase(expected.begin() + 64);
            expected.erase(expected.begin() + 63);
        } else expected.push_back({operation == 0 ? "next" : "overflow", 0});
        std::sort(expected.begin(), expected.end(), zset_less);
        require(same(contents(data, database, "z"), expected), "extended fault success values");
        long long expiry;
        require(keys.expiry_time(database, "z", expiry) && expiry == deadline,
            "extended fault success lost TTL");
    }
}

static void test_pop_compact_remainder() {
    auto database = memory();
    dao::paged_zset data;
    dao::json_key keys;
    const std::string key = "pop-remainder";
    for (bool maximum : {false, true}) {
        for (bool oversized : {false, true}) {
            std::vector<zset_member> seed;
            for (unsigned i = 0; i < 160; ++i) {
                auto member = std::to_string(i) + std::string(12, '\0');
                if (oversized && i == (maximum ? 78u : 80u)) member.resize(300, 'x');
                const auto score = double(i) - 80;
                seed.push_back({std::move(member), i == 80 ? -0.0 : score});
            }
            // Exercise infinity and negative zero across the reply and remainder.
            const auto count = 161u;
            seed.push_back({"first", -INFINITY});
            std::sort(seed.begin(), seed.end(), zset_less);
            dao::zadd_result added;
            require(data.add(database, key, seed, {}, added) &&
                added.added == count && keys.set_expiry(database, key, deadline, 0),
                "native pop remainder fixture");
            std::string bytes;
            zset_root before;
            require(database->get(key, bytes) &&
                zset_page_codec::decode_root(bytes, before) == map_page_status::ok,
                "pop remainder fixture must have pages");
            if (maximum) std::reverse(seed.begin(), seed.end());
            std::vector<zset_member> popped;
            if (maximum) {
                std::string selected;
                dao::zset_pop_result out;
                require(data.mpop(database, {"missing", key}, true, 81, selected, out)
                    && out.found && selected == key, "multi-key paged pop");
                popped.swap(out.values);
            } else {
                require(data.pop(database, key, 81, false, popped), "paged pop");
            }
            const std::vector<zset_member> expected(seed.begin(), seed.begin() + 81);
            require(same(popped, expected), "paged pop reply");
            seed.erase(seed.begin(), seed.begin() + 81);
            if (maximum) std::reverse(seed.begin(), seed.end());
            require(same(contents(data, database, key), seed), "paged pop survivors");
            long long expiry;
            require(keys.expiry_time(database, key, expiry) && expiry == deadline,
                "paged pop lost TTL");
            const bool packed = !oversized && database->supports_compact_zsets();
            require(database->get(key, bytes) && compact_zset::matches(bytes) == packed,
                "paged pop chose the wrong remainder encoding");
            if (packed) {
                require(database->transact_pages({key}, [&](page_transaction& tx) {
                    for (uint64_t id = 1; id <= before.last_id; ++id) {
                        std::string page;
                        if (tx.lookup(key, id, page) != db::lookup_status::missing) return false;
                    }
                    return true;
                }) == db::batch_status::ok, "paged pop left old pages");
            }
            require(data.pop(database, key, count, false, popped) && same(popped, seed),
                "paged pop cleanup");
        }
    }
}

static void test_compact_lifecycle() {
    auto database = memory();
    if (!database->supports_compact_zsets()) return;
    dao::paged_zset data;
    dao::zadd_result added;
    require(data.add(database, "compact", {{"a", 1}, {"b", 2}}, {}, added)
        && added.added == 2, "compact fixture");
    std::vector<zset_member> large;
    for (unsigned i = 0; i < compact_zset::max_members; ++i)
        large.push_back({"m" + std::to_string(10 + i), static_cast<double>(i)});
    std::string large_bytes;
    require(data.add(database, "large-compact", large, {}, added) &&
        added.added == static_cast<long long>(large.size()) &&
        database->get("large-compact", large_bytes) &&
        compact_zset::view(large_bytes).count() == large.size(),
        "compact root at the member limit");
    const compact_zset::view large_view(large_bytes);
    large_view.validate_fast();
    long long high_removed;
    require(data.remove(database, "large-compact", {large.back().member}, high_removed) &&
        high_removed == 1 && database->get("large-compact", large_bytes) &&
        compact_zset::view(large_bytes).count() == large.size() - 1,
        "compact removal above bit 31");
    require(data.add_one(database, "large-compact", large.back().member,
        large.back().score, {}, added) && added.added == 1 &&
        data.add_one(database, "large-compact", "overflow", 0, {}, added) &&
        added.added == 1 && database->get("large-compact", large_bytes) &&
        !compact_zset::matches(large_bytes), "insertion beyond the member limit promotes the root");
    dao::zadd_options options;
    options.xx = true;
    options.incr = true;
    require(data.add_one(database, "compact", "a", 3, options, added)
        && added.processed && added.score == 4, "compact increment");
    long long removed;
    require(data.remove(database, "compact", {"b"}, removed) && removed == 1,
        "compact removal");
    std::string bytes;
    require(database->get("compact", bytes) && compact_zset::matches(bytes),
        "small update promoted compact root");
    dao::json_key keys;
    std::string expiring, persisted;
    for (const auto expiry : {deadline, deadline + 1}) {
        require(keys.set_expiry(database, "compact", expiry, 0) &&
            database->get("compact", expiring) &&
            compact_zset::view(expiring).deadline() == expiry &&
            expiring.substr(13) == bytes.substr(5), "compact deadline edit");
    }
    require(keys.persist(database, "compact") &&
        database->get("compact", persisted) && persisted == bytes,
        "compact persist changed member bytes");
    auto corrupt = bytes;
    corrupt[5] = 1;
    require(database->set("corrupt-compact", corrupt) &&
        !keys.set_expiry(database, "corrupt-compact", deadline, 0) &&
        keys.status() == dao::dao_base::read_status::invalid_record &&
        database->get("corrupt-compact", persisted) && persisted == corrupt,
        "TTL edit accepted malformed compact offsets");
    root(database, "compact", 1);
}

int main() {
    {
        using namespace compact_zset;
        const std::string first(125, 'a'), middle(125, 'm'), last(125, 'z');
        std::vector<entry> entries{{first, 1}, {last, 1}};
        auto bytes = encode(entries, deadline);
        for (const auto& member : {middle, std::string(), std::string(125, char(255))}) {
            const view packed(bytes);
            packed.validate_fast();
            unsigned insertion = 0;
            require(packed.member_index(member, nullptr, &insertion) == packed.count() &&
                packed.insert_member(member, 1, insertion, bytes),
                "compact direct insert");
            entries.emplace_back(member, 1);
            require(bytes == encode(entries, deadline),
                "compact insert must preserve TTL, widths and tied rank order");
        }
        const std::string extra(255, 'q');
        {
            const view packed(bytes);
            unsigned insertion = 0;
            require(packed.member_index(extra, nullptr, &insertion) == packed.count() &&
                packed.insert_member(extra, 1, insertion, bytes),
                "compact insertion extends past 512 bytes");
        }
    }

    {
        std::vector<compact_zset::entry> entries{{"a", 0}, {"b", 1}, {"c", 2}};
        auto bytes = compact_zset::encode(entries, deadline);
        for (double score : {2.25, -3.5, 0.1, -128.0, 127.99609375, -32768.0,
            32767.0, 32768.0, -32769.0, -0.0, 2.0, 0.0}) {
            const compact_zset::view packed(bytes);
            packed.validate_fast();
            require(packed.replace_score(bytes, 1, score), "compact score width edit");
            entries[1].second = score;
            require(bytes == compact_zset::encode(entries, deadline),
                "compact score edit changed offsets, rank or TTL");
            const compact_zset::view updated(bytes);
            double decoded;
            updated.validate_fast();
            require(updated.find(std::string("b"), decoded) && decoded == score &&
                std::signbit(decoded) == std::signbit(score),
                "compact score edit lost precision");
        }
        require(compact_zset::payload_size({{"a", -32768}, {"b", 32767}}) == 8 &&
            compact_zset::payload_size({{"a", -32769}, {"b", 32768}}) == 20,
            "compact integer score widths");
        std::string legacy("PZR\3\1\0\0\x80", 8);
        std::string encoded_score;
        require(zset_page_codec::encode_value(2.25, encoded_score),
            "legacy compact score encoding");
        legacy += encoded_score + "a";
        const compact_zset::view previous(legacy);
        previous.validate_fast();
        double decoded;
        require(previous.find(std::string("a"), decoded) && decoded == 2.25,
            "legacy compact score decoding");
    }
    {
        std::vector<compact_zset::entry> entries{
            {std::string(255, 'a'), 0}, {std::string(255, 'b'), 1}};
        const auto narrow = compact_zset::encode({entries.front()});
        require(!(compact_zset::byte(narrow[4]) & compact_zset::wide_offsets_flag),
            "256-byte payload should use narrow offsets");
        for (unsigned i = 2; i < 48; ++i)
            entries.push_back({std::string(255, static_cast<char>('a' + i)),
                static_cast<double>(i)});
        auto wide = compact_zset::encode(entries);
        require(compact_zset::byte(wide[4]) & compact_zset::wide_offsets_flag,
            "12288-byte payload should use wide offsets");
        const compact_zset::view packed(wide);
        packed.validate_fast();
        const auto full = wide;
        require(!packed.insert_member("", 0, 0, wide) && wide == full,
            "oversized compact insertion must leave the root intact");
        require(packed.entries() == entries &&
            !packed.replace_score(wide, 0, 32767),
            "wide payload boundary or round trip");
        packed.replace_deadline(wide, deadline);
        const compact_zset::view expiring(wide);
        require(expiring.deadline() == deadline && expiring.entries() == entries,
            "wide offset deadline edit");
        entries.push_back({"", 0});
        require(compact_zset::try_encode(entries).empty(),
            "12289-byte payload should promote");
        entries = {{std::string(140, 'a'), 0}, {std::string(140, 'b'), 1}};
        wide = compact_zset::encode(entries);
        const compact_zset::view edit(wide);
        edit.validate_fast();
        require(edit.replace_score(wide, 0, 32767), "wide score width edit");
        entries[0].second = 32767;
        require(wide == compact_zset::encode(entries), "wide score edit changed offsets");
    }
    acl::acl_cpp_init();
    std::thread cache_model(test_compact_validation_cache);
    cache_model.join();
    test_extended_compact_codec();
    test_compact_single_erase();
    test_compact_score_moves();
    test_compact_rank_permutations();
    test_bulk_add_new_key();
    test_bulk_store_alias();
    test_batched_intersection_cardinality();
    test_native_score_moves();
    test_native_score_move_failure();
    test_model(); test_migration_and_keys(); test_expired_page_reclamation();
    test_failure_rollback(); test_complex_failures();
    test_invalid_records_and_sampling();
    test_pop_compact_remainder();
    test_compact_lifecycle();
    test_extended_compact_model();
    test_extended_compact_failures();
#if defined(HAS_TBB)
    use_tbb = true;
    test_bulk_store_alias();
    test_batched_intersection_cardinality();
    test_native_score_moves();
    test_native_score_move_failure();
    test_model(); test_migration_and_keys(); test_expired_page_reclamation();
    test_failure_rollback(); test_complex_failures();
    test_pop_compact_remainder();
    test_invalid_records_and_sampling();
    test_compact_lifecycle();
    test_extended_compact_model();
    test_extended_compact_failures();
#endif
    std::cout << "Native ZSet DAO, migration and key integration passed\n";
}
