#include "stdafx.h"
#include "common/redis_decimal.h"
#include "coder/json_record_codec.h"
#include "dao/paged_hash.h"
#include "dao/paged_string.h"
#include "dao/json/json_key.h"
#include "dao/json/json_string.h"
#include "db/mdb/mdb.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <locale.h>
#include <new>

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
static shared_db memory(bool tbb = false) {
    auto database = tbb ? db::create_mdb_tbb(4) : db::create_mdb(4);
    require(database && database->open(""), "memory open");
    return database;
}
static void test_decimal() {
    long double value = 7;
    std::string out;
    for (const auto& text : {"", " ", " 1", "1 ", "\t1", "1x", "+",
        "nan", "NaN(foo)", "1e99999", "1e-99999", "1,5"})
        require(!parse_redis_decimal(text, value) && value == 7,
            "invalid decimal accepted/changed output");
    require(!parse_redis_decimal(std::string("1\0", 2), value) &&
        !parse_redis_decimal(std::string(redis_decimal_limit, '0'), value),
        "decimal NUL/size bound");
    for (const auto& text : {"0.125", "+.125", "125e-3", "0x1p-3"})
        require(parse_redis_decimal(text, value) && value == .125L,
            "valid decimal rejected");
    require(parse_redis_decimal("-0", value) && std::signbit(value) &&
        format_redis_decimal(value, out) && out == "0", "negative zero");
    require(format_redis_decimal(.125L, out) && out == "0.125" &&
        format_redis_decimal(1000, out) && out == "1000" &&
        format_redis_decimal(-1e-30L, out) && out == "0",
        "human decimal formatting");
    require(parse_redis_decimal("Infinity", value) && std::isinf(value) &&
        !format_redis_decimal(value, out) && out.empty(), "infinite decimal");
    const auto maximum = std::numeric_limits<long double>::max();
    require(format_redis_decimal(maximum, out) &&
        out.size() < redis_decimal_limit && parse_redis_decimal(out, value) &&
        value == maximum, "maximum decimal buffer");
    char subnormal[128];
    const auto smallest = std::numeric_limits<long double>::denorm_min();
    const int size = std::snprintf(subnormal, sizeof(subnormal), "%La",
        smallest);
    require(size > 0 && parse_redis_decimal(subnormal, value) &&
        value == smallest, "nonzero subnormal rejected");
    // Thread-local formatting must ignore a caller's non-C numeric locale.
#ifndef _WIN32
    for (const char* name : {"fr_FR.UTF-8", "de_DE.UTF-8", "fr_FR.utf8"}) {
        locale_t other = newlocale(LC_NUMERIC_MASK, name, nullptr);
        if (!other) continue;
        const auto previous = uselocale(other);
        require(previous && format_redis_decimal(.125L, out) &&
            out == "0.125" && parse_redis_decimal(".125", value) &&
            value == .125L && uselocale(nullptr) == other,
            "numeric locale escaped thread guard");
        uselocale(previous); freelocale(other);
        break;
    }
#endif
}

static void test_commands(bool tbb) {
    auto database = memory(tbb);
    dao::paged_hash hash;
    dao::paged_string native_strings;
    dao::json_string strings;
    dao::json_key keys;
    std::string out, before, after;
    long long expiry;
    require(hash.hincrbyfloat(database, "hash", "", ".125", out) &&
        out == "0.125" && keys.set_expiry(database, "hash", deadline, 0) &&
        hash.hincrbyfloat(database, "hash", "", "-.125", out) && out == "0"
        && keys.expiry_time(database, "hash", expiry) && expiry == deadline,
        "native Hash float/TTL");
    require(strings.increment_float(database, "string", "0.25", out) &&
        out == "0.25" && keys.set_expiry(database, "string", deadline, 0) &&
        native_strings.increment_float(database, "string", "-.125", out) &&
        out == "0.125" && keys.expiry_time(database, "string", expiry) &&
        expiry == deadline, "String float/TTL");
    std::string maximum;
    require(format_redis_decimal(std::numeric_limits<long double>::max(),
        maximum) && strings.set(database, "string", maximum) &&
        database->get("string", before) &&
        !strings.increment_float(database, "string", maximum, out) &&
        out.empty() && strings.status() ==
        dao::dao_base::read_status::nonfinite_result &&
        database->get("string", after) && after == before,
        "finite String addition overflow changed value");
    require(hash.hset(database, "hash", "maximum", maximum) &&
        database->get("hash", before) &&
        !hash.hincrbyfloat(database, "hash", "maximum", maximum, out) &&
        out.empty() && hash.status() ==
        dao::dao_base::read_status::nonfinite_result &&
        database->get("hash", after) && after == before &&
        hash.hget(database, "hash", "maximum", out) && out == maximum,
        "finite Hash addition overflow changed value");
    require(database->get("hash", before), "snapshot Hash");
    for (const auto& delta : {"nan", "inf", "bad", "1e99999"}) {
        out = "stale";
        require(!hash.hincrbyfloat(database, "hash", "new", delta, out) &&
            out.empty() && database->get("hash", after) && after == before,
            "invalid Hash increment changed root/output");
    }
    require(hash.hset(database, "hash", "bad", "not-a-number") &&
        !hash.hincrbyfloat(database, "hash", "bad", "0", out) &&
        hash.status() == dao::dao_base::read_status::invalid_hash_float,
        "invalid stored Hash float");
    require(hash.hset(database, "hash", "infinite", "inf") &&
        !hash.hincrbyfloat(database, "hash", "infinite", "0", out) &&
        hash.status() == dao::dao_base::read_status::nonfinite_result,
        "nonfinite Hash result");
    require(!strings.increment_float(database, "hash", "bad", out) &&
        strings.status() == dao::dao_base::read_status::wrong_type &&
        !hash.hincrbyfloat(database, "string", "field", "bad", out) &&
        hash.status() == dao::dao_base::read_status::invalid_score,
        "String/Hash error precedence");
    require(keys.set_expiry(database, "hash", 0, 0) &&
        hash.hincrbyfloat(database, "hash", "fresh", "2", out) &&
        out == "2" && keys.expiry_time(database, "hash", expiry) &&
        expiry == -1, "expired Hash float rebuild");
}

class failing_db : public mdb {
public:
    failing_db() : mdb(4) {}
    bool fail_read = false, fail_write = false;
protected:
    // This fixture injects legacy dbset failure, not native page commits.
    bool dbsupports_pages() const override { return false; }
    lookup_status dblookup(const std::string& key, std::string& out) override {
        return fail_read ? lookup_status::error : mdb::dblookup(key, out);
    }
    bool dbset(const std::string& key, const std::string& value) override {
        return !fail_write && mdb::dbset(key, value);
    }
};

static void test_string_failures() {
    auto native = std::make_shared<failing_db>();
    shared_db database = native;
    require(database->open(""), "failure open");
    dao::json_string strings;
    dao::json_key keys;
    require(strings.set(database, "string", "0.125") &&
        keys.set_expiry(database, "string", deadline, 0), "failure seed");
    std::string before, after, out;
    require(database->get("string", before), "failure snapshot");
    native->fail_read = true;
    out = "stale";
    require(!strings.increment_float(database, "string", ".125", out) &&
        out.empty() && strings.status() ==
        dao::dao_base::read_status::storage_error, "failed float read hidden");
    native->fail_read = false;
    native->fail_write = true;
    out = "stale";
    require(!strings.increment_float(database, "string", ".125", out) &&
        out.empty() && strings.status() ==
        dao::dao_base::read_status::storage_error &&
        database->get("string", after) && after == before,
        "failed float write changed record/output");
    native->fail_write = false;
    long long expiry;
    require(keys.expiry_time(database, "string", expiry) && expiry == deadline,
        "failed float changed TTL");
}

class tracked_db : public mdb {
public:
    tracked_db() : mdb(4) {}
    size_t reads = 0, blobs = 0, writes = 0;
protected:
    lookup_status dblookup_page(const std::string& owner, uint64_t id,
        std::string& out) override {
        ++reads;
        const auto status = mdb::dblookup_page(owner, id, out);
        if (status == lookup_status::found && out.compare(0, 3, "PMB") == 0)
            ++blobs;
        return status;
    }
    batch_status dbapply_pages(const std::vector<batch_entry>& roots,
        const std::vector<page_change>& pages) override {
        writes += pages.size();
        return mdb::dbapply_pages(roots, pages);
    }
};
static void test_locality() {
    auto native = std::make_shared<tracked_db>();
    shared_db database = native;
    require(database->open(""), "tracked open");
    dao::paged_hash hash;
    std::map<std::string, std::string> fields;
    for (unsigned i = 0; i < 2000; ++i)
        fields[std::to_string(i)] = std::string(20000, 'x');
    fields["counter"] = "0.125";
    require(hash.hmset(database, "hash", fields) == 2001, "locality seed");
    native->reads = native->blobs = native->writes = 0;
    std::string out;
    require(hash.hincrbyfloat(database, "hash", "counter", ".125", out) &&
        out == "0.25" && native->reads < 80 && native->writes < 5 &&
        !native->blobs, "float increment scanned/rewrote Hash");
    native->reads = native->blobs = native->writes = 0;
    require(!hash.hincrbyfloat(database, "hash", "1000", "1", out) &&
        hash.status() == dao::dao_base::read_status::invalid_hash_float &&
        native->reads < 60 && !native->blobs && !native->writes,
        "oversized numeric field loaded value fragments");
}

static void test_rollback(bool tbb, bool legacy) {
    const std::string counter(40, 'c');
    value_record old;
    old.header.kind = value_kind::hash;
    old.header.expires_at_ms = deadline;
    old.hash_value = {{counter, "0.125"}, {"large", std::string(40000,'x')}};
    json_record_codec codec;
    std::string encoded;
    require(codec.encode(old, encoded) == codec_status::ok, "legacy encode");
    unsigned failures = 0;
    bool success = false;
    for (long budget = 0; budget < 1200; ++budget) {
        auto database = memory(tbb);
        dao::paged_hash hash;
        require(database->set("hash", encoded), "rollback seed");
        if (!legacy) {
            long long size;
            require(hash.hlen(database, "hash", size) && size == 2,
                "native rollback seed");
        }
        std::string before, after, out = "stale";
        require(database->get("hash", before), "rollback snapshot");
        allocation_budget = budget;
        bool ok = false;
        try {
            ok = hash.hincrbyfloat(database, "hash", counter, ".125", out);
        }
        catch (const std::bad_alloc&) {}
        allocation_budget = -1;
        if (ok) {
            require(out == "0.25", "rollback success value");
            success = true; break;
        }
        ++failures;
        require(out.empty() && database->get("hash", after) &&
            after == before, "failed float changed root/output");
        std::string value;
        if (legacy) {
            require(database->transact_pages({"hash"},
                [&](page_transaction& tx) {
                return tx.lookup("hash", 1, value) ==
                    db::lookup_status::missing;
            }) == db::batch_status::ok, "failed migration left pages");
        }
        require(hash.hget(database, "hash", counter, value) &&
            value == "0.125" && hash.hget(database, "hash", "large", value)
            && value == old.hash_value.at("large"),
            "failed float changed pages");
    }
    // Sweep every allocation failure up to the first successful operation.
    // Optimizations may reduce the number of allocations below ten.
    require(success && failures > 0, "float allocation rollback sweep");
    std::cout << "rollback tbb=" << tbb << " legacy=" << legacy
        << " failure points=" << failures << '\n';
}

int main() {
    acl::acl_cpp_init();
    test_decimal(); test_commands(false); test_locality();
    test_string_failures();
    test_rollback(false, false); test_rollback(false, true);
#ifdef HAS_TBB
    test_commands(true); test_rollback(true, false); test_rollback(true, true);
#endif
    std::cout << "Decimal increments, locality and rollback passed\n";
}
