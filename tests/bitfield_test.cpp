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
    require(database->open(""), "BITFIELD backend open");
    return database;
}
static bitfield_operation field(field_command command, unsigned bits,
    bool sign, uint64_t offset, long long value = 0,
    field_overflow overflow = field_overflow::wrap) {
    bitfield_operation operation;
    operation.command = command; operation.bits = bits; operation.sign = sign;
    operation.offset = offset; operation.value = value;
    operation.overflow = overflow;
    return operation;
}
static long long model_read(const std::string& bytes,
    const bitfield_operation& operation) {
    long long value = 0;
    for (unsigned i = 0; i < operation.bits; ++i) {
        const auto bit = operation.offset + i;
        const bool set = bit / 8 < bytes.size() &&
            (static_cast<unsigned char>(bytes[bit / 8]) &
                (1U << (7 - bit % 8)));
        value = value * 2 + set;
    }
    if (operation.sign && (value & (1LL << (operation.bits - 1))))
        value -= 1LL << operation.bits;
    return value;
}
static void model_write(std::string& bytes, const bitfield_operation& operation,
    long long value) {
    for (unsigned i = 0; i < operation.bits; ++i) {
        const auto bit = operation.offset + i;
        const auto mask = 1U << (7 - bit % 8);
        const auto byte = static_cast<unsigned char>(bytes[bit / 8]);
        bytes[bit / 8] = static_cast<char>(
            (static_cast<uint64_t>(value) >> (operation.bits - 1 - i)) & 1 ?
                byte | mask : byte & ~mask);
    }
}
static void byte_model(bool tbb) {
    auto database = memory(tbb);
    dao::paged_string data;
    std::string expected, actual;
    std::mt19937 random(67121);
    for (unsigned i = 0; i < 400; ++i) {
        auto operation = field(static_cast<field_command>(random() % 3),
            1 + random() % 24, random() % 2, random() % (20000 * 8),
            static_cast<long long>(random() % 200001) - 100000,
            static_cast<field_overflow>(random() % 3));
        const auto old = model_read(expected, operation);
        bitfield_reply answer;
        auto value = old;
        auto query = operation; query.command = field_command::get;
        std::vector<bitfield_operation> operations{operation};
        if (operation.command == field_command::get) answer.value = old;
        else {
            expected.resize(std::max(expected.size(), static_cast<size_t>(
                (operation.offset + operation.bits + 7) / 8)), '\0');
            const auto maximum = (1LL << (operation.bits -
                (operation.sign ? 1 : 0))) - 1;
            const auto minimum = operation.sign ? -maximum - 1 : 0;
            auto candidate = operation.command == field_command::set ?
                operation.value : old + operation.value;
            const bool below = candidate < minimum;
            const bool above = candidate > maximum || (!operation.sign &&
                operation.command == field_command::set && candidate < 0);
            if ((below || above) && operation.overflow == field_overflow::fail)
                answer.null = true;
            else {
                if ((below || above) &&
                    operation.overflow == field_overflow::saturate)
                    candidate = above ? maximum : minimum;
                auto raw = static_cast<uint64_t>(candidate) &
                    ((uint64_t(1) << operation.bits) - 1);
                value = static_cast<long long>(raw);
                if (operation.sign && (raw >> (operation.bits - 1)))
                    value -= 1LL << operation.bits;
                model_write(expected, operation, value);
                answer.value = operation.command == field_command::set ?
                    old : value;
            }
            operations.push_back(query);
        }
        std::vector<bitfield_reply> replies;
        require(data.bitfield(database, "model", operations, false, replies) &&
            replies.size() == operations.size() &&
            replies[0].null == answer.null &&
            (answer.null || replies[0].value == answer.value),
            "BITFIELD byte model reply");
        if (operations.size() == 2)
            require(!replies[1].null && replies[1].value == value,
                "BITFIELD sequential GET model");
        if (!expected.empty())
            require(data.get(database, "model", actual) && actual == expected,
                "BITFIELD byte model final bytes");
    }
}

static void legacy_and_limits(bool tbb) {
    auto database = memory(tbb);
    dao::paged_string data;
    dao::json_string old;
    dao::json_key keys;
    dao::set_options options; options.expires_at_ms = deadline;
    dao::set_result result;
    require(old.set(database, "old", std::string(20000, '\xff'), options,
        result), "BITFIELD legacy seed");
    std::string before, after;
    require(database->get("old", before), "BITFIELD legacy snapshot");
    std::vector<bitfield_reply> out;
    require(data.bitfield(database, "old", {
        field(field_command::get, 64, true, 16384 * 8 - 3),
        field(field_command::get, 63, false, 4294967295ULL)}, true, out) &&
        out.size() == 2 && out[0].value == -1 && out[1].value == 0 &&
        database->get("old", after) && after == before,
        "BITFIELD_RO migrated/materialized legacy input");
    require(data.bitfield(database, "old", {}, false, out) && out.empty() &&
        database->get("old", after) && after == before,
        "empty BITFIELD changed a key");
    require(!data.bitfield(database, "old", {
        field(field_command::set, 8, false, 0, 1)}, true, out) && out.empty() &&
        database->get("old", after) && after == before,
        "BITFIELD_RO permitted a write");
    require(data.bitfield(database, "new", {
        field(field_command::set, 1, false, 100, 2, field_overflow::fail)},
        false, out) && out[0].null && data.get(database, "new", after) &&
        after == std::string(13, '\0'), "failed field did not pre-grow key");
    uint64_t offset;
    require(!parse_field_offset("#2305843009213693952", 8, offset),
        "indexed field offset wrapped into range");
    long long expiry;
    require(keys.expiry_time(database, "old", expiry) && expiry == deadline,
        "BITFIELD limits changed TTL");
}

static void rollback(bool tbb, bool legacy) {
    bool success = false;
    unsigned failures = 0;
    for (long budget = 0; budget < 1000 && !success; ++budget) {
        auto database = memory(tbb);
        dao::paged_string data;
        dao::json_string old;
        const std::string initial(20001, '\x11');
        require(legacy ? old.set(database, "a", initial) :
            data.set(database, "a", initial), "BITFIELD rollback seed");
        std::string before, after;
        require(database->get("a", before), "BITFIELD rollback snapshot");
        std::vector<bitfield_operation> operations{
            field(field_command::set, 64, true, 16384 * 8 - 3, LLONG_MIN),
            field(field_command::get, 64, true, 16384 * 8 - 3),
            field(field_command::increment, 8, false, 50000 * 8, 1)};
        std::vector<bitfield_reply> out;
        bool ok = false;
        allocation_budget = budget;
        try { ok = data.bitfield(database, "a", operations, false, out); }
        catch (const std::bad_alloc&) {}
        allocation_budget = -1;
        if (ok) {
            success = true;
            require(out.size() == 3 && out[1].value == LLONG_MIN &&
                out[2].value == 1 && data.get(database, "a", after) &&
                after.size() == 50001 && after.back() == 1,
                "BITFIELD rollback final result");
        } else {
            ++failures;
            require(out.empty() && database->get("a", after) && before == after,
                "failed BITFIELD changed root or retained replies");
            require(database->transact_page_reads({"a"},
                [&](page_transaction& tx) {
                for (uint64_t id = legacy ? 1 : 3; id <= 4; ++id)
                    if (tx.lookup("a", id, after) != db::lookup_status::missing)
                        return false;
                return true;
            }) == db::batch_status::ok, "failed BITFIELD leaked pages");
            require(data.get(database, "a", after) && after == initial,
                "failed BITFIELD changed existing pages");
        }
    }
    require(success && failures > 10, "BITFIELD allocation sweep coverage");
}

class tracked_db : public mdb {
public:
    tracked_db() : mdb(4) {}
    size_t reads = 0, writes = 0;
protected:
    lookup_status dblookup_page(const std::string& key, uint64_t id,
        std::string& out) override {
        ++reads; return mdb::dblookup_page(key, id, out);
    }
    batch_status dbapply_pages(const std::vector<batch_entry>& roots,
        const std::vector<page_change>& pages) override {
        writes += pages.size(); return mdb::dbapply_pages(roots, pages);
    }
};
static void locality() {
    auto native = std::make_shared<tracked_db>();
    shared_db database = native;
    require(database->open(""), "BITFIELD tracked open");
    dao::paged_string data;
    require(data.set(database, "large", std::string(8 * 1024 * 1024, '\0')),
        "BITFIELD large seed");
    const auto write = field(field_command::set, 64, true,
        16384 * 8 - 3, LLONG_MIN);
    auto read = write; read.command = field_command::get;
    std::vector<bitfield_reply> out;
    native->reads = native->writes = 0;
    allocation_limit = 32768;
    const bool ok = data.bitfield(database, "large", {write, read}, false, out)
        && out.size() == 2 && out[0].value == 0 && out[1].value == LLONG_MIN &&
        data.bitfield(database, "large", {read}, true, out) &&
        out.size() == 1 && out[0].value == LLONG_MIN;
    allocation_limit = SIZE_MAX;
    require(ok && native->reads == 6 && native->writes == 2,
        "BITFIELD touched/materialized unrelated pages");
    require(database->transact_pages({"large"}, [](page_transaction& tx) {
        return tx.erase("large", 2);
    }) == db::batch_status::ok, "BITFIELD corrupt seed");
    require(!data.bitfield(database, "large", {
        field(field_command::set, 8, false, 0, 255), read}, false, out) &&
        out.empty(), "BITFIELD ignored missing page after a staged write");
    std::string bytes;
    require(data.getrange(database, "large", 0, 0, bytes) &&
        bytes == std::string(1, '\0'),
        "BITFIELD corrupt read published preceding writes");
}

static void field_tail(shared_db database) {
    dao::paged_string data;
    const auto limit = string_page_codec::write_bytes;
    // Intentionally omit unrelated prefix pages. Point operations validate
    // only accessed chunks, so this probes the boundary without 512-MiB data.
    require(database->transact_pages({"tail"}, [&](page_transaction& tx) {
        string_root root;
        root.length = limit; root.pages = string_page_codec::pages_for(limit);
        root.expires_at_ms = deadline;
        auto& entry = *tx.roots().find("tail");
        entry.action = db::update_action::write; entry.clear_pages = true;
        return string_page_codec::encode_root(root, entry.value) ==
            string_page_status::ok && tx.put("tail", root.pages,
                std::string(string_page_codec::page_bytes, '\0'));
    }) == db::batch_status::ok, "BITFIELD tail fixture");
    std::vector<bitfield_reply> out;
    require(data.bitfield(database, "tail", {
        field(field_command::set, 64, true, 4294967295ULL, LLONG_MIN),
        field(field_command::get, 64, true, 4294967295ULL)}, false, out) &&
        out.size() == 2 && out[0].value == 0 && out[1].value == LLONG_MIN,
        "BITFIELD could not extend its final field beyond 512 MiB");
    long long length;
    require(data.length(database, "tail", length) &&
        length == static_cast<long long>(limit + 8), "BITFIELD tail length");
    require(data.getbit(database, "tail", 4294967295ULL, length) && length == 1
        && !data.getbit(database, "tail", 4294967296ULL, length),
        "BITFIELD extension changed GETBIT offset bounds");
    require(!data.append(database, "tail", "", length),
        "APPEND accepted an oversized total length");
    require(!data.setrange(database, "tail", limit, "x", length),
        "SETRANGE accepted an oversized end offset");
    require(data.setrange(database, "tail", limit - 1, std::string(1, '\0'),
        length) &&
        length == static_cast<long long>(limit + 8),
        "SETRANGE could not update a valid prefix of a field-tail value");
    require(data.bitfield(database, "tail", {
        field(field_command::get, 64, true, 4294967295ULL)}, true, out) &&
        out.size() == 1 && out[0].value == 0,
        "field-tail prefix update did not reach field reads");
    dao::json_key keys;
    require(keys.expiry_time(database, "tail", length) && length == deadline,
        "BITFIELD tail changed exact TTL");
}

static void concurrent(bool tbb) {
    auto database = memory(tbb);
    dao::paged_string data;
    require(data.set(database, "shared", std::string(20001, '\0')),
        "BITFIELD concurrent seed");
    const auto increment = field(field_command::increment, 64, true,
        16384 * 8 - 1, 1);
    std::vector<long long> results[6];
    std::vector<std::thread> threads;
    for (unsigned n = 0; n < 6; ++n) threads.emplace_back([&, n] {
        dao::paged_string local;
        for (unsigned i = 0; i < 100; ++i) {
            std::vector<bitfield_reply> out;
            require(local.bitfield(database, "shared", {increment}, false,
                out) && out.size() == 1 && !out[0].null, "concurrent BITFIELD");
            results[n].push_back(out[0].value);
        }
    });
    for (auto& thread : threads) thread.join();
    std::vector<long long> all;
    for (const auto& result : results) all.insert(all.end(),
        result.begin(), result.end());
    std::sort(all.begin(), all.end());
    for (size_t i = 0; i < all.size(); ++i)
        require(all[i] == static_cast<long long>(i + 1),
            "lost field increment");
}

static void disk(const std::string& backend, const std::string& path) {
    const std::string key("field\0disk", 10);
    for (unsigned phase = 0; phase < 3; ++phase) {
        auto database = backend == "rdb" ? db::create_rdb() : db::create_wdb();
        require(database->open(path.c_str()), "BITFIELD disk open");
        dao::paged_string data;
        dao::json_key keys;
        std::vector<bitfield_reply> out;
        const auto read = field(field_command::get, 64, true, 16384 * 8 - 3);
        if (!phase) {
            require(data.set(database, key, std::string(20000, '\0')) &&
                keys.set_expiry(database, key, deadline, 0) &&
                data.bitfield(database, key, {
                    field(field_command::set, 64, true, read.offset, LLONG_MIN),
                    field(field_command::set, 8, false, 800, 200)}, false, out),
                "BITFIELD disk seed");
            field_tail(database);
        } else {
            require(data.bitfield(database, key, {read,
                field(field_command::get, 8, false, 800)}, true, out) &&
                out.size() == 2 && out[0].value ==
                    (phase == 1 ? LLONG_MIN : LLONG_MAX) && out[1].value == 200,
                "BITFIELD disk retained values");
            long long expiry;
            require(keys.expiry_time(database, key, expiry) &&
                expiry == deadline,
                "BITFIELD disk retained TTL");
            require(data.bitfield(database, "tail", {
                field(field_command::get, 64, true, 4294967295ULL)}, true,
                out) && out.size() == 1 && out[0].value == 0 &&
                data.length(database, "tail", expiry) &&
                expiry == static_cast<long long>(
                    string_page_codec::write_bytes + 8),
                "BITFIELD disk retained extended tail");
            if (phase == 1)
                require(data.bitfield(database, key, {
                    field(field_command::increment, 64, true, read.offset, -1),
                    field(field_command::increment, 8, false, 800, 100,
                        field_overflow::fail)}, false, out) && out[1].null,
                    "BITFIELD disk overflow update");
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
        byte_model(tbb); legacy_and_limits(tbb); field_tail(memory(tbb));
        rollback(tbb, false); rollback(tbb, true); concurrent(tbb);
    }
    locality();
    std::cout << "BITFIELD model/readonly/rollback/locality/concurrency "
        "passed\n";
}
