#include "stdafx.h"
#include "db/expiry_index.h"
#include "db/database_pool.h"
#include "db/mdb/mdb.h"
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
int main() {
    databases();
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
    for (unsigned i = 0; i < 20000; ++i)
        require(index.set(std::to_string(i), 5000), "sampling setup");
    value = index.snapshot(1000);
    require(value.count == 20001 && value.average >= 4000,
        "bounded expiry sample");
    std::cout << "INFO expiry count, bounded samples and "
        "TTL arithmetic passed\n";
}
