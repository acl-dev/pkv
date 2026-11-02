#include "stdafx.h"
#include "dao/paged_string.h"
#include "dao/json/json_string.h"
#include "dao/json/json_key.h"
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
static const int64_t deadline = 4102444800123LL;
static void require(bool ok, const char* message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}
static shared_db memory(bool tbb) {
    auto database = tbb ? db::create_mdb_tbb(4) : db::create_mdb(4);
    require(database->open(""), "bitmap backend open");
    return database;
}
static void model(bool tbb) {
    auto database = memory(tbb);
    dao::paged_string data;
    dao::json_key keys;
    std::string expected, out;
    std::mt19937 random(0xB17);
    long long previous, actual;
    for (unsigned i = 0; i < 500; ++i) {
        const auto offset = random() % (40000 * 8);
        const auto byte = offset / 8;
        const auto mask = static_cast<unsigned char>(1U << (7 - offset % 8));
        const int bit = random() % 2;
        const auto old = byte < expected.size() ?
            ((static_cast<unsigned char>(expected[byte]) & mask) != 0) : false;
        require(data.setbit(database, "bits", offset, bit, previous) &&
            previous == old, "SETBIT model old bit");
        if (byte >= expected.size()) expected.resize(byte + 1, '\0');
        const auto value = static_cast<unsigned char>(expected[byte]);
        expected[byte] = static_cast<char>(bit ? value | mask : value & ~mask);
        require(data.getbit(database, "bits", offset, actual) && actual == bit,
            "GETBIT model");
        if (i % 25 == 0) {
            require(data.get(database, "bits", out) && out == expected,
                "bitmap model full bytes");
            require(keys.set_expiry(database, "bits", deadline, 0), "TTL set");
            long long count = 0;
            for (unsigned char ch : expected)
                for (unsigned n = 0; n < 8; ++n) count += (ch >> n) & 1;
            require(data.bitcount(database, "bits", bitmap_range(), actual) &&
                actual == count, "BITCOUNT model");
        }
    }
    require(data.get(database, "bits", out) && out == expected,
        "bitmap final bytes");
    long long expiry;
    require(keys.expiry_time(database, "bits", expiry) && expiry == deadline,
        "bitmap exact TTL retention");
    require(data.getbit(database, "bits", 4294967295LL, actual) && !actual,
        "maximum GETBIT offset");
    require(!data.setbit(database, "bits", 4294967296LL, 1, actual) &&
        data.status() == dao::dao_base::read_status::bit_offset,
        "invalid bit offset");
    require(!data.setbit(database, "bits", 0, 2, actual) &&
        data.status() == dao::dao_base::read_status::invalid_bit,
        "invalid bit value");
}

static void concurrency(bool tbb) {
    auto database = memory(tbb);
    dao::paged_string data;
    require(data.set(database, "shared", std::string(16385, '\0')),
        "bitmap concurrent seed");
    std::vector<std::thread> workers;
    for (unsigned thread = 0; thread < 8; ++thread)
        workers.emplace_back([&, thread] {
            dao::paged_string local;
            for (unsigned i = 0; i < 100; ++i) {
                long long previous;
                // Disjoint bits in common bytes catch lost byte updates.
                require(local.setbit(database, "shared", i * 8 + thread,
                    1, previous) && previous == 0, "concurrent bitmap update");
            }
        });
    for (auto& worker : workers) worker.join();
    long long count;
    require(data.bitcount(database, "shared", bitmap_range(), count) &&
        count == 800, "concurrent bitmap lost bits");
}

class tracked_db : public mdb {
public:
    tracked_db() : mdb(4) {}
    size_t reads = 0, writes = 0;
    bool fail_write = false;
protected:
    lookup_status dblookup_page(const std::string& key, uint64_t id,
        std::string& out) override {
        ++reads; return mdb::dblookup_page(key, id, out);
    }
    batch_status dbapply_pages(const std::vector<batch_entry>& roots,
        const std::vector<page_change>& pages) override {
        writes += pages.size();
        if (fail_write) return batch_status::error;
        return mdb::dbapply_pages(roots, pages);
    }
};
static void locality() {
    auto native = std::make_shared<tracked_db>();
    shared_db database = native;
    require(database->open(""), "tracked bitmap open");
    dao::paged_string data;
    const long long size = 8 * 1024 * 1024;
    require(data.set(database, "large", std::string(size, '\0')),
        "bitmap large seed");
    long long out;
    bitmap_range range;
    range.bits = range.end_given = true;
    range.start = 16384 * 8 - 1; range.end = range.start + 1;
    native->reads = native->writes = 0;
    allocation_limit = 32768;
    const bool ok = data.getbit(database, "large", 0, out) && out == 0 &&
        data.setbit(database, "large", 16384 * 8, 1, out) && out == 0 &&
        data.bitcount(database, "large", range, out) && out == 1 &&
        data.bitpos(database, "large", 1, range, out) &&
        out == 16384 * 8;
    allocation_limit = SIZE_MAX;
    require(ok && native->reads == 6 && native->writes == 1,
        "bitmap small operations touched unrelated pages");
    native->reads = native->writes = 0;
    allocation_limit = 32768;
    const bool scan = data.bitcount(database, "large", bitmap_range(), out) &&
        out == 1 && data.bitpos(database, "large", 1, bitmap_range(), out) &&
        out == 16384 * 8;
    allocation_limit = SIZE_MAX;
    require(scan && native->reads == 514 && !native->writes,
        "bitmap scan materialized value or failed to stop early");
    native->fail_write = true;
    require(!data.setbit(database, "large", 16384 * 8, 0, out) && out == 0,
        "failed bitmap commit status");
    native->fail_write = false;
    require(data.getbit(database, "large", 16384 * 8, out) && out == 1,
        "failed bitmap commit changed data");
}

static void rollback(bool tbb, bool legacy) {
    unsigned failures = 0;
    bool success = false;
    for (long budget = 0; budget < 1000 && !success; ++budget) {
        auto database = memory(tbb);
        dao::paged_string data;
        dao::json_string old;
        dao::set_options options;
        options.expires_at_ms = deadline;
        dao::set_result result;
        const std::string initial(40000, '\0');
        require((legacy ? old.set(database, "bits", initial, options, result) :
            data.set(database, "bits", initial, options, result)),
            "bitmap rollback seed");
        std::string before, after;
        require(database->get("bits", before), "bitmap rollback root");
        long long previous;
        allocation_budget = budget;
        bool ok = false;
        try {
            ok = data.setbit(database, "bits", 60000 * 8 + 3, 1, previous);
        } catch (const std::bad_alloc&) {}
        allocation_budget = -1;
        if (ok) {
            success = true;
            require(previous == 0 && data.get(database, "bits", after) &&
                after.size() == 60001 && after[60000] == 16 &&
                after.substr(0, 60000) == std::string(60000, '\0'),
                "bitmap rollback success bytes");
        } else {
            ++failures;
            require(previous == 0 && database->get("bits", after) &&
                before == after, "failed bitmap changed root");
            require(database->transact_page_reads({"bits"},
                [&](page_transaction& tx) {
                    std::string bytes;
                    return tx.lookup("bits", legacy ? 1 : 4, bytes) ==
                        db::lookup_status::missing;
                }) == db::batch_status::ok, "failed bitmap leaked pages");
            require(data.get(database, "bits", after) && after == initial,
                "failed bitmap changed pages");
        }
    }
    require(success && failures > 10, "bitmap allocation sweep coverage");
}

static void corruption(bool tbb) {
    auto database = memory(tbb);
    dao::paged_string data;
    require(data.set(database, "broken", std::string(20000, '\0')),
        "bitmap corrupt seed");
    require(database->transact_pages({"broken"}, [](page_transaction& tx) {
        return tx.erase("broken", 1);
    }) == db::batch_status::ok, "bitmap corrupt remove page");
    std::string before, after;
    require(database->get("broken", before), "bitmap corrupt root");
    long long out;
    require(!data.setbit(database, "broken", 0, 1, out) && out == 0 &&
        data.status() == dao::dao_base::read_status::invalid_record,
        "SETBIT treated missing page as zero");
    require(database->get("broken", after) && after == before,
        "failed SETBIT changed corrupt root");
    require(!data.bitcount(database, "broken", bitmap_range(), out) &&
        out == 0, "BITCOUNT ignored missing page");
    bitmap_range range;
    range.start = 16384;
    require(data.bitcount(database, "broken", range, out) && out == 0,
        "BITCOUNT validated unrelated page");
    range.start = 20000;
    require(data.bitpos(database, "broken", 0, range, out) && out == -1,
        "BITPOS read pages for an empty range");
    before[3] = 99;
    require(database->set("future", before), "bitmap future seed");
    require(!data.getbit(database, "future", 0, out) && out == 0 &&
        data.status() == dao::dao_base::read_status::unsupported_format,
        "GETBIT accepted a future root");
}

static void disk(const std::string& backend, const std::string& path) {
    for (unsigned phase = 0; phase < 3; ++phase) {
        auto database = backend == "rdb" ? db::create_rdb() : db::create_wdb();
        require(database->open(path.c_str()), "bitmap disk open");
        dao::paged_string data;
        dao::json_key keys;
        const std::string key("disk\0bitmap", 11);
        long long out;
        if (phase == 0) {
            require(data.setbit(database, key, 20000 * 8 + 7, 1, out) && !out,
                "bitmap disk initial growth");
            require(keys.set_expiry(database, key, deadline, 0),
                "bitmap disk expiry");
        } else {
            require(data.getbit(database, key, 20000 * 8 + 7, out) &&
                out == (phase == 1 ? 1 : 0), "bitmap disk retained bit");
            require(data.bitcount(database, key, bitmap_range(), out) &&
                out == 1, "bitmap disk retained count");
            require(keys.expiry_time(database, key, out) && out == deadline,
                "bitmap disk retained TTL");
            if (phase == 1)
                require(data.setbit(database, key, 20000 * 8 + 7, 0, out) &&
                    out == 1 && data.setbit(database, key, 16384 * 8, 1, out)
                    && !out, "bitmap disk changed pages");
            else require(data.bitpos(database, key, 1, bitmap_range(), out) &&
                out == 16384 * 8, "bitmap disk retained position");
        }
    }
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
        model(tbb); concurrency(tbb); corruption(tbb);
        rollback(tbb, false); rollback(tbb, true);
    }
    locality();
    std::cout << "native bitmap model/concurrency/locality/rollback passed\n";
}
