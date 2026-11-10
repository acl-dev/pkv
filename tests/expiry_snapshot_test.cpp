#include "stdafx.h"
#include "db/expiry_index.h"
#include "db/database_pool.h"
#include "db/mdb/mdb.h"
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <limits>

static void require(bool ok, const char* reason) {
    if (!ok) { std::cerr << reason << '\n'; std::exit(1); }
}
static void databases() {
    auto zero = std::make_shared<pkv::mdb>(4);
    require(zero->open(""), "DB zero open");
    size_t opened = 0;
    pkv::database_pool pool(zero, 65536, [&](size_t index) {
        ++opened;
        require(index == 3 || index == 5, "opened unknown empty database");
        auto value = std::make_shared<pkv::mdb>(4);
        require(value->open(""), "stored DB open");
        return value;
    }, {3, 3});
    auto selected = pool.materialized();
    require(opened == 1 && selected.size() == 2 && selected[0].first == 0 &&
        selected[1].first == 3, "stored database discovery/deduplication");
    require(bool(pool.get(5)), "selected DB open");
    require(pool.materialized().size() == 3 && opened == 2,
        "newly selected DB discovery");
    pkv::database_pool failed(zero, 16,
        [](size_t) { return pkv::shared_db(); }, {7});
    bool threw = false;
    try { failed.materialized(); } catch (const std::runtime_error&) {
        threw = true;
    }
    require(threw, "unavailable stored DB silently omitted");
}
static void collisions() {
    pkv::expiry_index index;
    std::vector<std::string> keys;
    for (unsigned i = 0; keys.size() < 96; ++i) {
        auto key = "expiry-collision:" + std::to_string(i);
        // Start at the last slot of every table up to 128 slots.
        if (std::hash<std::string>()(key) % 8192 == 8191)
            keys.push_back(std::move(key));
    }
    for (size_t i = 0; i < keys.size(); ++i)
        require(index.set(keys[i], 500 + i), "colliding expiry insertion");
    for (size_t i = 0; i < keys.size(); i += 2) index.erase(keys[i]);
    for (size_t i = 1; i < keys.size(); i += 2)
        require(index.set(keys[i], 100), "refresh through erased hash slots");
    for (size_t i = 0; i < keys.size(); i += 4)
        require(index.set(keys[i], 2000), "refill expired hash slots");
    std::vector<std::string> due, expected;
    index.due(100, keys.size(), due);
    for (size_t i = 1; i < keys.size(); i += 2) expected.push_back(keys[i]);
    std::sort(due.begin(), due.end());
    std::sort(expected.begin(), expected.end());
    require(due == expected && index.size() == 72 && !index.dirty(),
        "colliding expiry heap or hash table diverged");
#if defined(__SIZEOF_INT128__)
    const auto snapshot = index.snapshot(100);
    require(snapshot.count == 72 && snapshot.average == 633,
        "expiry deadline total diverged after updates and removals");
#endif
    for (const auto& key : keys) index.erase(key);
    require(!index.size() && index.set(keys.back(), 0) &&
        index.due(0, 1, due) && due == std::vector<std::string>{keys.back()},
        "colliding expiry index did not recover after shrinking");
}
int main() {
    databases();
    collisions();
    pkv::expiry_index index;
    require(index.set("a", 1500) && index.set("b", 2500), "expiry setup");
    auto value = index.snapshot(1000);
    require(value.count == 2 && value.average == 1000, "TTL average");
    value = index.snapshot(2000);
    require(value.count == 2 && value.average == 250, "expired TTL clamp");
    index.erase("a"); index.set("b", -1);
    require(!index.snapshot(0).count, "expiry removal");
    require(index.set("huge", std::numeric_limits<int64_t>::max()),
        "huge TTL setup");
    value = index.snapshot(std::numeric_limits<int64_t>::min());
    require(value.count == 1 && value.average ==
        std::numeric_limits<uint64_t>::max(), "TTL subtraction overflow");
    require(index.set("lower", std::numeric_limits<int64_t>::max() - 16) &&
        index.set("middle", std::numeric_limits<int64_t>::max() - 5),
        "wide TTL sum setup");
    value = index.snapshot(std::numeric_limits<int64_t>::min());
    require(value.count == 3 && value.average ==
        std::numeric_limits<uint64_t>::max() - 7, "wide TTL sum and division");
    index.erase("lower"); index.erase("middle");
    for (unsigned i = 0; i < 20000; ++i)
        require(index.set(std::to_string(i), 5000), "sampling setup");
    value = index.snapshot(1000);
    require(value.count == 20001 && value.average >= 4000,
        "bounded expiry sample");
    std::cout << "INFO expiry count, bounded samples and "
        "TTL arithmetic passed\n";
}
