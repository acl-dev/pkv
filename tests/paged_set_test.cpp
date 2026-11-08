#include "stdafx.h"
#include "dao/paged_set.h"
#include "dao/json/json_set.h"
#include "dao/json/json_zset.h"
#include "dao/json/json_key.h"
#include "dao/json/json_scan.h"
#include "coder/json_record_codec.h"
#include "coder/compact_set.h"
#include "db/mdb/mdb.h"
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
static void require(bool ok, const char* reason) {
    if (!ok) { std::cerr << reason << '\n'; std::exit(1); }
}

class tracking_db : public mdb {
public:
    tracking_db() : mdb(4) {}
    size_t reads = 0, writes = 0, largest = 0;
    size_t directories_read = 0;
protected:
    lookup_status dblookup_page(const std::string& key, uint64_t id,
        std::string& out) override {
        ++reads;
        const auto status = mdb::dblookup_page(key, id, out);
        if (status == lookup_status::found && out.compare(0, 3, "PMD") == 0)
            ++directories_read;
        return status;
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

static bool seed_paged_set(shared_db& database, const std::string& key,
    const std::vector<std::string>& members) {
    return database->transact_page(key, [&](page_transaction& tx) {
        map_store store(tx, key, map_kind::set);
        store.initialize();
        for (const auto& member : members) {
            bool added;
            if (!store.put(member, "", true, added) || !added) return false;
        }
        return store.finish();
    }) == db::batch_status::ok;
}

static void root(shared_db database, const std::string& key, size_t size) {
    std::string bytes;
    require(database->get(key, bytes) == (size != 0), "root existence");
    if (!size) return;
    if (compact_set::matches(bytes)) {
        require(compact_set::view(bytes).count() == size, "compact Set size");
        return;
    }
    map_root record;
    require(map_page_codec::decode_root(bytes, record) ==
        map_page_status::ok && record.kind == map_kind::set &&
        record.elements == size, "Set stored as whole value");
}

static void test_compact_reads() {
    auto tracked = memory();
    shared_db database = tracked;
    dao::paged_set data;
    std::vector<std::string> fixed{"abc", "def", "xyz"};
    auto encoded = compact_set::encode(fixed);
    require(compact_set::view(encoded).contains(std::string("def")) &&
        !compact_set::view(encoded).contains(std::string("de")),
        "fixed-width Set lookup");
    const std::string first(255, 'a'), second(255, 'b'), third(255, 'c'), fourth(255, 'd');
    std::vector<std::string> single{first};
    const auto narrow = compact_set::encode(single);
    std::string widened, wide, overflow;
    require(compact_set::view(narrow).insert(second, widened) &&
        (compact_set::byte(widened[4]) & compact_set::wide_offsets_flag),
        "256-byte Set should widen at 512 bytes");
    std::vector<std::string> wide_members{first, second, third, fourth};
    wide = compact_set::encode(wide_members);
    require(!wide.empty() &&
        (compact_set::byte(wide[4]) & compact_set::wide_offsets_flag),
        "1024-byte Set should use wide offsets");
    compact_set::view(wide).replace_deadline(wide, 4102444800123LL);
    const compact_set::view packed(wide);
    require(packed.deadline() == 4102444800123LL && packed.contains(first) &&
        packed.contains(second) && packed.insert(std::string(), overflow) &&
        compact_set::byte(overflow[3]) == 8 &&
        compact_set::view(overflow).deadline() == 4102444800123LL,
        "legacy Set payload growth preserves deadline");
    const auto remaining = packed.without(1, 3);
    require(compact_set::view(remaining).contains(second),
        "wide Set removal");
    encoded[8] = 2;
    require(database->set("bad-fixed", encoded), "invalid fixed-width Set seed");
    long long bad_size;
    require(!data.size(database, "bad-fixed", bad_size) &&
        data.status() == dao::dao_base::read_status::invalid_record,
        "fixed-width Set accepted a bad length");
    long long added;
    std::vector<std::string> many;
    for (unsigned i = 0; i < compact_set::max_members; ++i)
        many.push_back("m" + std::to_string(10 + i));
    auto full = compact_set::encode(many);
    const compact_set::view full_set(full);
    require(full_set.count() == many.size() &&
        !full_set.insert(std::string("overflow"), overflow), "compact Set member limit");
    require(database->set("full", full) &&
        data.change(database, "full", {many.back(), many.back()}, false, added) &&
        added == 1 && database->get("full", full) &&
        compact_set::view(full).count() == many.size() - 1 &&
        !compact_set::view(full).contains(many.back()), "compact Set removal above bit 31");
    require(data.change_one(database, "full", many.back(), true, added) && added == 1,
        "refill the compact Set");
    many.push_back("overflow");
    require(data.change(database, "full", many, true, added) && added == 1 &&
        database->get("full", full) && !compact_set::matches(full),
        "large batch promotes the compact Set");
    root(database, "full", many.size());
    require(data.change(database, "compact", {"", "alpha", "beta"}, true, added) &&
        added == 3, "compact Set seed");
    require(data.change(database, "other", {"", "alpha", "gamma"}, true, added) &&
        added == 3, "compact intersection seed");
    std::string before, after;
    require(database->get("compact", before) && compact_set::matches(before),
        "expected compact Set");
    const auto reads = tracked->reads, writes = tracked->writes;
    long long intersection;
    require(data.intersection_size(database, {"compact", "other"}, 0, intersection) &&
        intersection == 2 &&
        data.intersection_size(database, {"compact", "other"}, 1, intersection) &&
        intersection == 1 &&
        data.intersection_size(database, {"compact", "compact"}, 0, intersection) &&
        intersection == 3, "compact intersection cardinality");
    std::set<std::string> algebra;
    require(data.algebra(database, {"compact", "other", "missing"},
        dao::set_operation::unite, nullptr, algebra) &&
        algebra == std::set<std::string>{"", "alpha", "beta", "gamma"},
        "compact union");
    require(data.algebra(database, {"compact", "other", "other"},
        dao::set_operation::intersect, nullptr, algebra) &&
        algebra == std::set<std::string>{"", "alpha"}, "compact intersection");
    require(data.algebra(database, {"compact", "missing", "other"},
        dao::set_operation::difference, nullptr, algebra) &&
        algebra == std::set<std::string>{"beta"}, "compact difference");
    require(data.algebra(database, {"compact", "other", "compact"},
        dao::set_operation::difference, nullptr, algebra) && algebra.empty(),
        "compact difference with repeated first key");
    require(contents(data, database, "compact") ==
        std::set<std::string>{"", "alpha", "beta"}, "compact member enumeration");
    std::vector<long long> membership;
    require(data.membership(database, "compact", {"alpha", "missing", ""}, membership) &&
        membership == std::vector<long long>{1, 0, 1}, "compact batch membership");
    dao::json_scan scan;
    scan_options options;
    options.has_match = true;
    options.pattern = "a*";
    scan_page page;
    require(scan.open_collection(database, "compact", value_kind::set) &&
        scan.collection(options, page) && page.cursor == 0 &&
        page.values == std::vector<std::string>{"alpha"}, "compact scan");
    dao::set_sample_result sample;
    require(data.sample(database, "compact", -8, false, sample) &&
        sample.found && sample.values.size() == 8, "compact repeated sample");
    require(database->get("compact", after) && after == before &&
        tracked->reads == reads && tracked->writes == writes,
        "compact reads touched pages");

    dao::json_key key;
    require(key.set_expiry(database, "compact", deadline, 0), "compact Set expiry");
    for (const auto* member : {"aardvark", "alpine", "zulu"}) {
        require(data.change_one(database, "compact", member, true, added) &&
            added == 1 && database->get("compact", before) &&
            compact_set::matches(before) &&
            compact_set::view(before).deadline() == deadline,
            "single member compact insertion");
        require(data.change_one(database, "compact", member, true, added) &&
            added == 0 && database->get("compact", after) && before == after,
            "duplicate compact insertion");
    }
    long long removed;
    require(data.change(database, "compact", {"alpha", "missing", "alpha"},
        false, removed) && removed == 1 &&
        database->get("compact", after) && compact_set::matches(after) &&
        compact_set::view(after).deadline() == deadline &&
        contents(data, database, "compact") ==
            std::set<std::string>{"", "aardvark", "alpine", "beta", "zulu"},
        "compact Set partial removal");
    require(data.change_one(database, "compact", "missing", false, removed) &&
        removed == 0 && database->get("compact", before) && before == after,
        "compact Set no-op removal");
    require(data.change(database, "compact", {"beta", "", "zulu", "alpine",
        "aardvark"}, false, removed) && removed == 5 &&
        !database->get("compact", after) &&
        tracked->reads == reads && tracked->writes == writes,
        "compact Set final removal touched pages");
    require(data.change(database, "compact", {"", "alpha", "beta"}, true, added) &&
        key.set_expiry(database, "compact", deadline, 0), "compact pop seed");
    require(data.sample(database, "compact", 2, true, sample) &&
        sample.values.size() == 2 && sample.values[0] != sample.values[1] &&
        database->get("compact", after) && compact_set::matches(after) &&
        compact_set::view(after).count() == 1 &&
        compact_set::view(after).deadline() == deadline, "compact partial pop");
    require(data.sample(database, "compact", 3, true, sample) &&
        sample.values.size() == 1 && !database->get("compact", after) &&
        tracked->reads == reads && tracked->writes == writes,
        "compact final pop touched pages");

    const std::string target = "store";
    require(seed_paged_set(database, target, {"old"}) &&
        key.set_expiry(database, target, deadline, 0), "Set STORE target seed");
    require(data.algebra(database, {"other", "missing"},
        dao::set_operation::unite, &target, algebra) && algebra.size() == 3 &&
        database->get(target, after) && compact_set::matches(after) &&
        compact_set::view(after).deadline() == -1, "compact STORE replacement");
    require(data.algebra(database, {target, "other"},
        dao::set_operation::intersect, &target, algebra) && algebra.size() == 3 &&
        database->get(target, after) && compact_set::matches(after),
        "compact STORE aliased input");
    require(data.algebra(database, {target, "other"},
        dao::set_operation::difference, &target, algebra) && algebra.empty() &&
        !database->get(target, after), "empty compact STORE removes target");

    require(data.change(database, "compact", {"alpha", "beta"}, true, added) &&
        key.set_expiry(database, "compact", deadline, 0) &&
        key.set_expiry(database, "other", deadline - 1, 0), "compact move seed");
    const auto move_reads = tracked->reads, move_writes = tracked->writes;
    bool moved;
    require(data.move(database, "compact", "other", "alpha", moved) && moved &&
        data.move(database, "compact", "other", "beta", moved) && moved &&
        !database->get("compact", after) && database->get("other", after) &&
        compact_set::matches(after) && compact_set::view(after).deadline() == deadline - 1 &&
        contents(data, database, "other") ==
            std::set<std::string>{"", "alpha", "beta", "gamma"},
        "compact move with duplicate and new members");
    require(data.move(database, "other", "compact", "beta", moved) && moved &&
        database->get("compact", before) && compact_set::matches(before) &&
        compact_set::view(before).deadline() == -1 &&
        database->get("other", after) && compact_set::matches(after) &&
        compact_set::view(after).deadline() == deadline - 1 &&
        tracked->reads == move_reads && tracked->writes == move_writes,
        "compact move into missing target");
}

static void test_compact_set_member_order() {
    auto compare = [](const std::string& a, const std::string& b) {
        require(compact_set::short_member_less(a, b) == (a < b),
            "compact member comparison differs from binary string order");
        require(compact_set::short_member_less(b, a) == (b < a),
            "reversed compact member comparison differs from binary order");
    };
    std::mt19937 random(3047);
    for (size_t a_size = 0; a_size <= 65; ++a_size) {
        for (size_t b_size = 0; b_size <= 65; ++b_size) {
            std::string a(a_size, '\0'), b(b_size, '\0');
            for (auto& c : a) c = static_cast<char>(random());
            for (auto& c : b) c = static_cast<char>(random());
            compare(a, b);
            const auto common = std::min(a.size(), b.size());
            b.replace(0, common, a.data(), common);
            compare(a, b);
        }
    }
    for (size_t length = 7; length <= 33; ++length) {
        std::string a(length, static_cast<char>(0x80));
        compare(a, a);
        for (size_t position = 0; position < length; ++position) {
            for (unsigned changed : {0U, 127U, 129U, 255U}) {
                auto b = a;
                b[position] = static_cast<char>(changed);
                compare(a, b);
            }
        }
    }
}

static void test_extended_compact_set() {
    std::vector<std::string> binary;
    for (unsigned i = 0; i < 128; ++i)
        binary.emplace_back(1, static_cast<char>((i * 73) % 256));
    auto narrow_binary = compact_set::encode(binary);
    require(!(compact_set::byte(narrow_binary[4]) & compact_set::wide_offsets_flag) &&
        compact_set::view(narrow_binary).count() == 128,
        "128 binary members should fit one-byte offsets");
    for (const auto& item : binary)
        require(compact_set::view(narrow_binary).contains(item), "binary narrow member lost");
    compact_set::view(narrow_binary).replace_deadline(narrow_binary, 4102444800123LL);
    require(compact_set::view(narrow_binary).count() == 128 &&
        compact_set::view(narrow_binary).deadline() == 4102444800123LL,
        "128 count bit confused with expiry flag");
    std::vector<std::string> members;
    for (unsigned i = 0; i < 128; ++i) {
        char text[4]; std::snprintf(text, sizeof(text), "%03u", i);
        members.emplace_back(text);
    }
    auto first = std::vector<std::string>(members.begin(), members.begin() + 63);
    auto bytes = compact_set::encode(first, 4102444800123LL);
    require(compact_set::byte(bytes[3]) == 6, "small Sets changed format");
    const auto legacy = bytes;
    for (unsigned i = 63; i < members.size(); ++i) {
        std::string next;
        require(compact_set::view(bytes).insert(members[i], next) && !next.empty(),
            "extended Set insertion");
        bytes.swap(next);
        const compact_set::view value(bytes);
        require(value.count() == i + 1 && value.contains(members[i]) &&
            value.deadline() == 4102444800123LL, "extended count, member or TTL");
        if (i == 63) require(!(compact_set::byte(bytes[4]) &
            compact_set::wide_offsets_flag), "64 fixed members should remain narrow");
        if (i == 64) require(compact_set::byte(bytes[4]) &
            compact_set::wide_offsets_flag, "65 fixed members should widen");
    }
    require(compact_set::byte(bytes[3]) == 8, "extended Set tag");
    for (unsigned i = 0; i < 4; ++i)
        require(compact_set::view(bytes).contains(members[127]), "warm extended validation");
    auto corrupt = bytes;
    // Keep address, length and all admission samples unchanged. A full-byte
    // comparison must still detect this unsorted middle member.
    const compact_set::view validated(corrupt);
    const auto middle = validated.at(40).data() - corrupt.data();
    corrupt[static_cast<size_t>(middle)] = 'z';
    bool rejected_change = false;
    try { const compact_set::view invalid(corrupt); (void)invalid; }
    catch (const std::runtime_error&) { rejected_change = true; }
    require(rejected_change, "validation cache trusted unchanged samples");
    bool optional_cache_ok = false;
    std::thread fresh_worker([&] {
        allocation_budget = 0;
        try {
            const compact_set::view first(bytes), second(bytes), third(bytes);
            optional_cache_ok = first.count() == 128 && second.count() == 128 &&
                third.contains(members[127]);
        } catch (const std::bad_alloc&) {}
        allocation_budget = -1;
    });
    fresh_worker.join();
    require(optional_cache_ok, "optional validation cache made a valid read fail");
    compact_set::erase_mask mask;
    mask.set(63); mask.set(64); mask.set(127);
    const auto trimmed = compact_set::view(bytes).without(mask, 125);
    const compact_set::view kept(trimmed);
    require(kept.count() == 125 && !kept.contains(members[63]) &&
        !kept.contains(members[64]) && !kept.contains(members[127]) &&
        kept.contains(members[126]), "extended erase bitmap lost high members");
    auto ttl = bytes;
    compact_set::view(ttl).replace_deadline(ttl, -1);
    require(compact_set::view(ttl).deadline() == -1, "extended PERSIST");
    compact_set::view(ttl).replace_deadline(ttl, 4102444800999LL);
    require(compact_set::view(ttl).deadline() == 4102444800999LL,
        "extended expiry insertion");

    std::vector<std::string> large;
    for (char c = 'a'; c <= 'p'; ++c) large.emplace_back(255, c);
    auto full_payload = compact_set::encode(large);
    std::string overflow;
    require(!full_payload.empty() &&
        !compact_set::view(full_payload).insert(std::string(), overflow),
        "extended payload cap");
    large.resize(5);
    auto invalid_legacy = compact_set::encode(large);
    invalid_legacy[3] = 6;
    invalid_legacy[4] = static_cast<char>(compact_set::byte(invalid_legacy[4]) | 5);
    invalid_legacy.erase(5, 1);
    auto invalid_flags = bytes; invalid_flags[4] |= 1;
    auto invalid_count = bytes; invalid_count[5] = static_cast<char>(129);
    for (const auto& invalid : {invalid_legacy, invalid_flags, invalid_count,
            bytes.substr(0, 6)}) {
        bool rejected = false;
        try { const compact_set::view value(invalid); (void)value; }
        catch (const std::runtime_error&) { rejected = true; }
        require(rejected, "extended decoder relaxed legacy or structural checks");
    }

    shared_db database = memory();
    dao::paged_set data;
    long long changed;
    require(database->set("wide", bytes) && data.change(database, "wide",
        {members[63], members[64], members[127], members[127]}, false, changed) &&
        changed == 3 && contents(data, database, "wide").size() == 125,
        "extended DAO multi removal");
    bool moved;
    require(data.move(database, "wide", "small", members[126], moved) && moved &&
        contents(data, database, "small") == std::set<std::string>{members[126]},
        "extended high-index SMOVE");
    dao::set_sample_result sample;
    require(data.sample(database, "wide", 65, true, sample) &&
        sample.values.size() == 65 &&
        std::set<std::string>(sample.values.begin(), sample.values.end()).size() == 65 &&
        contents(data, database, "wide").size() == 59, "extended SPOP bitmap");
    for (const auto& member : sample.values)
        require(!contents(data, database, "wide").count(member), "SPOP left a selected member");

    bool failed = false, succeeded = false;
    for (long budget = 0; budget < 200 && !succeeded; ++budget) {
        require(database->set("boundary", legacy), "legacy boundary seed");
        allocation_budget = budget;
        bool ok = false;
        try { ok = data.change_one(database, "boundary", members[63], true, changed); }
        catch (const std::bad_alloc&) {}
        allocation_budget = -1;
        std::string after;
        require(database->get("boundary", after), "boundary root lost");
        if (ok) {
            succeeded = true;
            require(compact_set::view(after).count() == 64, "boundary publication");
        } else {
            failed = true;
            require(after == legacy, "failed format transition changed root");
        }
    }
    require(failed && succeeded, "format transition fault sweep incomplete");
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

static void test_grouped_add() {
    auto tracked = memory();
    shared_db database = tracked;
    dao::paged_set data;
    std::vector<std::string> seed;
    for (unsigned i = 0; i < 4096; ++i)
        seed.push_back("seed:" + std::to_string(i));
    long long changed;
    require(data.change(database, "set", seed, true, changed) &&
        changed == 4096, "grouped SADD seed");
    std::string bytes;
    map_root metadata;
    require(database->get("set", bytes) &&
        map_page_codec::decode_root(bytes, metadata) == map_page_status::ok &&
        metadata.grouped, "grouped SADD layout");

    std::set<std::string> expected(seed.begin(), seed.end());
    std::vector<std::string> batch;
    for (unsigned i = 0; i < 64; ++i) {
        auto member = "new:" + std::to_string(i);
        expected.insert(member);
        batch.push_back(member);
        batch.push_back(seed[i]);
        batch.push_back(member);
    }
    require(data.change(database, "set", batch, true, changed) &&
        changed == 64 && contents(data, database, "set") == expected,
        "grouped SADD across directory pages");
    tracked->writes = 0;
    require(data.change(database, "set", batch, true, changed) &&
        changed == 0 && tracked->writes == 0, "grouped duplicate SADD");
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
    tracked->writes = 0;
    require(data.sample(database, "set", LLONG_MAX, true, out) &&
        out.values.size() == 9870 && tracked->writes == 0,
        "pop all without rewriting individual pages");
    root(database, "set", 0);
    require(data.sample(database, "set", -3, false, out) && !out.found &&
        out.values.empty(), "missing sample");

    members.resize(1100);
    require(data.change(database, "set", members, true, count) &&
        count == 1100 && data.sample(database, "set", 1099, true, out),
        "pop across emptied directory groups");
    const std::set<std::string> popped(out.values.begin(), out.values.end());
    require(popped.size() == 1099 &&
        data.sample(database, "set", 1, true, out) && out.values.size() == 1 &&
        !popped.count(out.values.front()), "pop through the updated directory");
    root(database, "set", 0);
}

static void test_migration_scan_copy() {
    shared_db database = memory(), destination = memory();
    json_record_codec codec(1);
    value_record record;
    record.header.kind = value_kind::set;
    record.header.expires_at_ms = deadline;
    for (unsigned i = 0; i < 1101; ++i)
        record.set_value.insert(std::to_string(i));
    record.set_value.insert(std::string(1024 * 1024, 'x'));
    std::string bytes;
    require(codec.encode(record, bytes) == codec_status::ok &&
        database->set("set", bytes), "legacy seed");
    dao::paged_set data;
    long long count;
    require(data.size(database, "set", count) && count == 1102,
        "legacy migration");
    root(database, "set", 1102);
    map_root metadata;
    require(database->get("set", bytes) && map_page_codec::decode_root(bytes,
        metadata) == map_page_status::ok &&
        metadata.grouped && metadata.expires_at_ms == deadline,
        "grouped migration TTL");
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
    require(contents(data, destination, "renamed").size() == record.set_value.size() + 1,
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

static void test_rank_selection(bool grouped) {
    shared_db database = memory();
    dao::paged_set data;
    std::vector<std::string> members = {"", std::string("a\0b", 3),
        std::string(40000, 'x')};
    for (unsigned i = 0; i < (grouped ? 1100U : 200U); ++i)
        members.push_back(std::to_string(i));
    const std::set<std::string> expected(members.begin(), members.end());
    members.push_back(members.front());
    members.push_back(members[1]);
    members.push_back(members[2]);
    long long changed;
    require(data.change(database, "set", members, true, changed) &&
        changed == static_cast<long long>(expected.size()) &&
        contents(data, database, "set") == expected, "rank seed");
    require(database->transact_pages({"set"}, [&](page_transaction& tx) {
        map_store map(tx, "set", map_kind::set);
        if (!map.open() || map.metadata().grouped != grouped) return false;
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
        while (!ordered.empty()) {
            const size_t rank = ordered.size() / 2;
            std::string member;
            require(map.take_set_rank(rank, member) && member == ordered[rank],
                "subtree rank removal");
            ordered.erase(ordered.begin() + rank);
            require(map.metadata().elements == ordered.size(),
                "subtree count after rank removal");
        }
        return map.finish();
    }) == db::batch_status::ok, "rank traversal");
    std::string remaining;
    require(!database->get("set", remaining), "final rank retained root");
}

static void test_full_set_validation() {
    shared_db database = memory();
    dao::paged_set data;
    const std::string key = "set";
    long long changed;
    require(seed_paged_set(database, key, {"a", "b", "c"}),
        "full set validation seed");
    require(database->transact_page(key, [&](page_transaction& tx) {
        map_root root;
        map_node node;
        std::string bytes;
        if (map_page_codec::decode_root(tx.roots().find(key)->value, root) !=
            map_page_status::ok || tx.lookup(key, root.tree, bytes) !=
            db::lookup_status::found || map_page_codec::decode_node(bytes, node) !=
            map_page_status::ok) return false;
        ++node.height;
        return map_page_codec::encode_packed_node(node, bytes) ==
            map_page_status::ok && tx.put(key, node.id, std::move(bytes));
    }) == db::batch_status::ok, "corrupt stored set height");
    require(!data.visit_members(database, key,
        [](uint64_t, const std::string&) {}), "accepted invalid set height");
    dao::set_sample_result sample;
    require(!data.sample(database, key, 3, false, sample) &&
        sample.values.empty(), "returned a partial corrupt sample");
    require(!data.sample(database, key, 2, true, sample) &&
        sample.values.empty(), "accepted a corrupt partial pop");
    require(!data.sample(database, key, 3, true, sample) &&
        sample.values.empty(), "accepted a corrupt full pop");
    require(data.size(database, key, changed) && changed == 3,
        "failed full pop erased the set");
    std::set<std::string> result;
    require(!data.algebra(database, {key}, dao::set_operation::unite,
        nullptr, result) && result.empty(), "returned corrupt union output");

    const std::string shared = "shared-fragments";
    const auto left = "a" + std::string(20000, 'x');
    const auto right = "b" + std::string(20000, 'x');
    require(data.change(database, shared, {left, right, "inline"}, true, changed),
        "shared fragment fixture");
    require(database->transact_page(shared, [&](page_transaction& tx) {
        map_root root;
        if (map_page_codec::decode_root(tx.roots().find(shared)->value, root) !=
            map_page_status::ok) return false;
        map_fragment first, second;
        for (uint64_t id = 1; id <= root.last_id; ++id) {
            std::string bytes;
            map_fragment fragment;
            if (tx.lookup(shared, id, bytes) != db::lookup_status::found ||
                map_page_codec::decode_fragment(bytes, fragment) != map_page_status::ok ||
                fragment.offset != 0) continue;
            if (fragment.bytes.front() == 'a') first = std::move(fragment);
            else if (fragment.bytes.front() == 'b') second = std::move(fragment);
        }
        if (!first.next || !second.next) return false;
        second.next = first.next;
        std::string bytes;
        return map_page_codec::encode_fragment(second, bytes) == map_page_status::ok &&
            tx.put(shared, second.id, std::move(bytes));
    }) == db::batch_status::ok, "share stored fragment suffix");
    require(data.visit_members(database, shared, [](uint64_t, const std::string&) {}),
        "shared suffix changed the stored members");
    require(!data.sample(database, shared, 3, true, sample) && sample.values.empty() &&
        data.size(database, shared, changed) && changed == 3,
        "full pop accepted a shared fragment or committed a prefix");
}

static void test_large_store_alias() {
    shared_db database = memory();
    dao::paged_set data;
    const std::string key = "destination";
    std::vector<std::string> members;
    for (unsigned i = 0; i < 1100; ++i)
        members.push_back(std::to_string(i));
    long long changed;
    require(data.change(database, key, members, true, changed) &&
        data.change(database, "other", {std::string(40000, 'x')}, true,
            changed), "large STORE seed");
    std::set<std::string> expected(members.begin(), members.end());
    expected.insert(std::string(40000, 'x'));
    std::set<std::string> result;
    require(data.algebra(database, {key, "other"}, dao::set_operation::unite,
        &key, result) && result == expected &&
        contents(data, database, key) == expected, "large aliased STORE");
}

static void test_bulk_stage_owner_isolation() {
    shared_db database = memory();
    dao::paged_set data;
    long long changed;
    require(seed_paged_set(database, "a", {"old"}),
        "seed bulk stage source");
    require(database->transact_pages({"a", "z"}, [&](page_transaction& tx) {
        std::string old_page, staged;
        if (tx.lookup("a", 1, old_page) != db::lookup_status::found) return false;
        map_store target(tx, "z", map_kind::set);
        target.initialize();
        const std::vector<std::string> members = {"new"};
        if (!target.build_set([&](size_t i) -> const std::string& {
            return members[i];
        }, members.size(), changed) || !target.finish()) return false;
        require(tx.lookup("a", 1, staged) == db::lookup_status::found &&
            staged == old_page, "bulk stage shadowed a different owner");
        if (!tx.reset_pages("a")) return false;
        require(tx.lookup("z", 1, staged) == db::lookup_status::found,
            "reset cleared a different owner's bulk stage");
        tx.roots().find("a")->action = db::update_action::write;
        return tx.put("a", 1, old_page);
    }) == db::batch_status::ok &&
        contents(data, database, "a") == std::set<std::string>{"old"} &&
        contents(data, database, "z") == std::set<std::string>{"new"},
        "bulk stage spill changed owner addresses");
}

static void test_grouped_algebra_large_bucket() {
    shared_db database = memory();
    dao::paged_set data;
    const std::string large(140 * 1024, 'x');
    std::vector<std::string> left{large}, right{large};
    for (unsigned i = 0; i < 1200; ++i) {
        left.push_back(std::to_string(i));
        right.push_back(std::to_string(i + 1100));
    }
    long long changed;
    require(data.change(database, "left", left, true, changed) &&
        data.change(database, "right", right, true, changed),
        "seed grouped algebra");
    std::set<std::string> expected{large}, actual;
    for (unsigned i = 1100; i < 1200; ++i)
        expected.insert(std::to_string(i));
    require(data.algebra(database, {"left", "right"},
        dao::set_operation::intersect, nullptr, actual) && actual == expected,
        "grouped intersection with oversized scratch bucket");
    const std::vector<std::string> inputs{"left", "right"};
    require(data.intersection_size(database, inputs, 0, changed) &&
        changed == 101 && data.intersection_size(database, inputs, 1, changed) &&
        changed == 1 && data.intersection_size(database, inputs, 1201, changed) &&
        changed == 101, "grouped SINTERCARD with and without an early limit");
    expected.clear();
    for (unsigned i = 0; i < 1100; ++i)
        expected.insert(std::to_string(i));
    require(data.algebra(database, {"left", "right"},
        dao::set_operation::difference, nullptr, actual) && actual == expected,
        "grouped difference with oversized scratch bucket");
}

static void test_intersection_limit_directory_reads() {
    auto tracked = memory();
    shared_db database = tracked;
    dao::paged_set data;
    std::vector<std::string> members;
    for (unsigned i = 0; i < 1500; ++i) members.push_back(std::to_string(i));
    long long count;
    require(data.change(database, "left", members, true, count) &&
        data.change(database, "right", members, true, count),
        "seed early grouped intersection");
    tracked->directories_read = 0;
    require(data.intersection_size(database, {"left", "right"}, 1, count) &&
        count == 1 && tracked->directories_read <= 4,
        "LIMIT 1 read unvisited Set directory groups");
    tracked->reads = 0;
    require(data.intersection_size(database, {"left", "left", "left"}, 0, count) &&
        count == 1500 && data.intersection_size(database, {"left"}, 1, count) &&
        count == 1 && tracked->reads == 0,
        "self intersection should only read Set metadata");
}

static void test_early_grouping(bool tbb) {
    shared_db database;
#if defined(HAS_TBB)
    if (tbb) database = db::create_mdb_tbb(4);
#else
    (void)tbb;
#endif
    if (!database) database = db::create_mdb(4);
    require(database->open(""), "early group database");
    std::vector<std::string> members;
    for (unsigned i = 0; i < 257; ++i) {
        // Include embedded NUL/high bytes and a fragmented member.
        members.push_back(std::string("member\0\xff", 8) + std::to_string(i));
    }
    members[31].append(40000, 'x');
    require(database->transact_page("set", [&](page_transaction& tx) {
        map_store store(tx, "set", map_kind::set);
        store.initialize(deadline);
        for (size_t i = 0; i < 256; ++i) {
            bool added = false;
            if (!store.put(members[i], "", true, added) || !added) return false;
        }
        return !store.metadata().grouped && store.finish();
    }) == db::batch_status::ok, "early group seed");
    using snapshot = std::map<uint64_t, std::string>;
    auto pages = [&]() {
        snapshot out;
        require(database->transact_page_reads({"set"}, [&](page_transaction& tx) {
            const auto* entry = tx.roots().find("set");
            map_root root;
            if (!entry || map_page_codec::decode_root(entry->value, root) !=
                    map_page_status::ok) return false;
            out.emplace(0, entry->value);
            for (uint64_t id = 1; id <= root.last_id; ++id) {
                std::string bytes;
                const auto status = tx.lookup("set", id, bytes);
                if (status == db::lookup_status::error) return false;
                if (status == db::lookup_status::found)
                    out.emplace(id, std::move(bytes));
            }
            return true;
        }) == db::batch_status::ok, "early group snapshot");
        return out;
    };
    const auto before = pages();
    dao::paged_set data;
    bool failed = false, succeeded = false;
    for (long budget = 0; budget < 4096 && !succeeded; ++budget) {
        long long changed = -1;
        bool ok = false;
        allocation_budget = budget;
        try { ok = data.change(database, "set", {members.back()}, true, changed); }
        catch (const std::bad_alloc&) {}
        allocation_budget = -1;
        if (ok) {
            require(changed == 1, "early group insertion result");
            succeeded = true;
        } else {
            failed = true;
            require(pages() == before, "early group failure changed roots/pages");
        }
    }
    require(failed && succeeded, "early group fault sweep incomplete");
    const std::set<std::string> expected(members.begin(), members.end());
    require(contents(data, database, "set") == expected, "early group members");
    require(database->transact_page_reads({"set"}, [&](page_transaction& tx) {
        map_store store(tx, "set", map_kind::set);
        if (!store.open() || !store.metadata().grouped ||
            store.metadata().elements != 257 ||
            store.metadata().expires_at_ms != deadline) return false;
        std::vector<std::string> order;
        if (!store.visit([&](const std::string& member, const std::string&) {
            order.push_back(member); return true;
        }, false)) return false;
        for (size_t i = 0; i < order.size(); ++i) {
            bool found = false;
            std::string member;
            if (!store.select(i, member, found) || !found || member != order[i])
                return false;
        }
        return order.size() == 257;
    }) == db::batch_status::ok, "early group rank/TTL");
    long long changed;
    require(data.change(database, "set", {members.back()}, true, changed) &&
        changed == 0 && contents(data, database, "set") == expected,
        "early group duplicate insertion");
}

static void test_set_repack(bool tbb) {
    shared_db database;
#if defined(HAS_TBB)
    if (tbb) database = db::create_mdb_tbb(4);
#else
    (void)tbb;
#endif
    if (!database) database = db::create_mdb(4);
    require(database->open(""), "repack database");
    std::vector<std::string> members;
    for (unsigned i = 0; i < 257; ++i)
        members.push_back("member:" + std::to_string(i) + std::string(12, 'x'));
    members[31] = std::string("binary\0", 7) + std::string(193, '\xff');
    auto seed = [&](const std::string& key) {
        require(database->transact_page(key, [&](page_transaction& tx) {
            map_store store(tx, key, map_kind::set);
            store.initialize(deadline);
            for (const auto& member : members) {
                bool added;
                if (!store.put(member, "", true, added) || !added) return false;
            }
            return store.finish();
        }) == db::batch_status::ok, "repack seed");
    };
    seed("set");
    auto snapshot = [&](const std::string& key) {
        std::map<uint64_t, std::string> result;
        require(database->transact_page_reads({key}, [&](page_transaction& tx) {
            const auto* entry = tx.roots().find(key);
            map_root root;
            if (map_page_codec::decode_root(entry->value, root) != map_page_status::ok)
                return false;
            result.emplace(0, entry->value);
            for (uint64_t i = 1; i <= root.last_id; ++i) {
                std::string bytes;
                const auto status = tx.lookup(key, i, bytes);
                if (status == db::lookup_status::error) return false;
                if (status == db::lookup_status::found) result.emplace(i, bytes);
            }
            return true;
        }) == db::batch_status::ok, "repack snapshot");
        return result;
    };
    const auto before = snapshot("set");
    std::vector<std::string> removed(members.begin() + 33, members.end());
    dao::paged_set data;
    bool failed = false, succeeded = false;
    for (long budget = 0; budget < 4096 && !succeeded; ++budget) {
        long long changed = -1;
        bool ok = false;
        allocation_budget = budget;
        try { ok = data.change(database, "set", removed, false, changed); }
        catch (const std::bad_alloc&) {}
        allocation_budget = -1;
        if (ok) {
            require(changed == 224, "repack removed count");
            succeeded = true;
        } else {
            failed = true;
            require(snapshot("set") == before, "repack failure changed raw pages");
        }
    }
    require(failed && succeeded, "repack fault sweep incomplete");
    std::string bytes;
    require(database->get("set", bytes) && compact_set::matches(bytes) &&
        compact_set::view(bytes).count() == 33 &&
        compact_set::view(bytes).deadline() == deadline &&
        contents(data, database, "set") ==
            std::set<std::string>(members.begin(), members.begin() + 33),
        "repack contents or TTL");
    require(database->transact_page_reads({"set"}, [&](page_transaction& tx) {
        for (const auto& page : before) {
            if (!page.first) continue;
            std::string value;
            if (tx.lookup("set", page.first, value) != db::lookup_status::missing)
                return false;
        }
        return true;
    }) == db::batch_status::ok, "repack left native pages");
    long long changed;
    require(data.change(database, "set", removed, true, changed) && changed == 224 &&
        contents(data, database, "set") ==
            std::set<std::string>(members.begin(), members.end()), "repack regrowth");
    dao::set_sample_result sample;
    require(data.sample(database, "set", 129, true, sample) &&
        sample.values.size() == 129 && database->get("set", bytes) &&
        compact_set::matches(bytes) && compact_set::view(bytes).count() == 128,
        "SPOP repack");
    auto all = contents(data, database, "set");
    all.insert(sample.values.begin(), sample.values.end());
    require(all == std::set<std::string>(members.begin(), members.end()),
        "SPOP repack lost members");

    seed("move");
    removed.assign(members.begin() + 129, members.end());
    require(data.change(database, "move", removed, false, changed) && changed == 128,
        "SMOVE repack seed");
    const auto move_before = snapshot("move");
    failed = succeeded = false;
    for (long budget = 0; budget < 4096 && !succeeded; ++budget) {
        bool ok = false, moved = false;
        allocation_budget = budget;
        try { ok = data.move(database, "move", "move-target", members[0], moved); }
        catch (const std::bad_alloc&) {}
        allocation_budget = -1;
        if (ok) {
            require(moved, "SMOVE repack result");
            succeeded = true;
        } else {
            failed = true;
            require(snapshot("move") == move_before &&
                !database->get("move-target", bytes), "SMOVE repack partial commit");
        }
    }
    require(failed && succeeded && database->get("move", bytes) &&
        compact_set::matches(bytes) && compact_set::view(bytes).count() == 128 &&
        compact_set::view(bytes).deadline() == deadline &&
        contents(data, database, "move") ==
            std::set<std::string>(members.begin() + 1, members.begin() + 129) &&
        contents(data, database, "move-target") == std::set<std::string>{members[0]},
        "SMOVE repack contents/TTL");

    members[0].append(40000, 'z');
    seed("wide");
    removed.assign(members.begin() + 1, members.end());
    require(data.change(database, "wide", removed, false, changed) && changed == 256 &&
        database->get("wide", bytes) && !compact_set::matches(bytes) &&
        contents(data, database, "wide") == std::set<std::string>{members[0]},
        "oversized member should retain paged format");
}

static void test_allocation_rollback() {
    for (unsigned operation = 0; operation < 4; ++operation) {
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
                else ok = data.sample(database, "source",
                    operation == 2 ? 2 : 1, true, sample);
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
    test_compact_reads();
    test_extended_compact_set();
    test_compact_set_member_order();
    test_failed_migration_and_ttl();
    test_mixed_page_reads();
    test_rank_selection(false);
    test_rank_selection(true);
    test_early_grouping(false);
    test_set_repack(false);
#if defined(HAS_TBB)
    test_early_grouping(true);
    test_set_repack(true);
#endif
    test_allocation_rollback();
    test_full_set_validation();
    test_large_store_alias();
    test_bulk_stage_owner_isolation();
    test_grouped_algebra_large_bucket();
    test_intersection_limit_directory_reads();
    test_grouped_add();
    test_sampling_and_locality();
    test_migration_scan_copy();
    test_errors_and_replacement();
    std::cout << "Native Set model, sampling and migration passed\n";
}
