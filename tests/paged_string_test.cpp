#include "stdafx.h"
#include "dao/paged_string.h"
#include "dao/json/json_string.h"
#include "dao/json/json_key.h"
#include "dao/paged_hash.h"
#include "coder/json_record_codec.h"
#include "db/mdb/mdb.h"
#include <cstdlib>
#include <iostream>
#include <new>
#include <random>

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
static bool use_tbb = false;
static const int64_t deadline = 4102444800123LL;
static void require(bool ok, const char* message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}
static shared_db memory() {
    auto database = use_tbb ? db::create_mdb_tbb(4) : db::create_mdb(4);
    require(database && database->open(""), "memory open");
    return database;
}
static void root(shared_db database, const std::string& key) {
    std::string bytes;
    if (!database->get(key, bytes)) return;
    string_root value;
    require(bytes.size() <= 1056 &&
        string_page_codec::decode_root(bytes, value) == string_page_status::ok,
        "whole String persisted instead of PSR root");
    std::unique_ptr<record_view> view;
    size_t length;
    require(database->value_codec().decode_stored(bytes, view) ==
        codec_status::ok && view && view->paged_payload() &&
        view->header().kind == value_kind::string &&
        view->string_length(length) && length == value.length &&
        !view->valid_payload(), "native String metadata view");
}
static void test_model() {
    auto database = memory(), reference = memory();
    dao::paged_string data;
    dao::json_string json;
    std::mt19937 random(51973);
    for (unsigned step = 0; step < 500; ++step) {
        const auto value = random() % 3 ? std::to_string(random() % 50) :
            std::string(20000, static_cast<char>(random()));
        const auto choice = random() % 7;
        bool first, second;
        if (choice == 0) {
            dao::set_options options;
            options.when = static_cast<dao::set_options::condition>(
                random() % 3);
            options.return_old = random() % 2;
            options.keep_ttl = random() % 2;
            if (!options.keep_ttl && random() % 2)
                options.expires_at_ms = deadline;
            dao::set_result a, b;
            first = data.set(database, "key", value, options, a);
            second = json.set(reference, "key", value, options, b);
            require(a.applied == b.applied && a.had_old == b.had_old &&
                a.old_value == b.old_value, "conditional SET result");
        } else if (choice == 1 || choice == 2 || choice == 3) {
            long long a, b;
            if (choice == 1) {
                first = data.append(database, "key", value, a);
                second = json.append(reference, "key", value, b);
            } else if (choice == 2) {
                const auto offset = random() % 40000;
                first = data.setrange(database, "key", offset, value, a);
                second = json.setrange(reference, "key", offset, value, b);
            } else {
                const auto delta = static_cast<long long>(random() % 10) - 5;
                first = data.increment(database, "key", delta, false, a);
                second = json.increment(reference, "key", delta, false, b);
            }
            if (first && second) require(a == b, "String numeric result");
        } else if (choice == 4) {
            dao::get_options options;
            options.expiry = random() % 2 ? dao::get_options::persist :
                dao::get_options::erase;
            dao::string_result a, b;
            first = data.getex(database, "key", options, a);
            second = json.getex(reference, "key", options, b);
            require(a.found == b.found && a.value == b.value, "GETEX result");
        } else {
            std::string a, b;
            if (choice == 5) {
                const auto start = static_cast<long long>(random() % 80000)
                    - 40000;
                const auto end = start + random() % 50000;
                first = data.getrange(database, "key", start, end, a);
                second = json.getrange(reference, "key", start, end, b);
            } else {
                first = data.increment_float(database, "key", ".125", a);
                second = json.increment_float(reference, "key", ".125", b);
            }
            if (first && second) require(a == b, "String bytes result");
        }
        require(first == second && (first || data.status() == json.status()),
            "String success/error status");
        std::string a, b;
        first = data.get(database, "key", a);
        second = json.get(reference, "key", b);
        require(first == second && (!first || a == b), "String model bytes");
        root(database, "key");
    }
}

static void test_migration_and_mget() {
    auto database = memory();
    dao::paged_string data;
    dao::json_string legacy;
    dao::json_key keys;
    dao::paged_hash hash;
    const std::string value(100000, '\0');
    require(legacy.set(database, "legacy", value) &&
        keys.set_expiry(database, "legacy", deadline, 0) &&
        data.set(database, "native", value) &&
        hash.hset(database, "hash", "field", "value"), "MGET seed");
    std::string before, after;
    require(database->get("legacy", before), "legacy snapshot");
    std::vector<dao::string_result> out;
    const std::vector<std::string> names = {"native", "legacy", "native",
        "hash", "absent"};
    require(data.mget(database, names,
        out) && out.size() == 5 && out[0].found && out[0].value == value &&
        out[1].found && out[1].value == value && out[2].value == value &&
        !out[3].found && !out[4].found && database->get("legacy", after) &&
        before == after, "MGET native/legacy/read-only snapshot");
    dao::set_options options;
    options.when = dao::set_options::only_missing; options.return_old = true;
    dao::set_result changed;
    require(data.set(database, "legacy", "ignored", options, changed) &&
        !changed.applied && changed.had_old && changed.old_value == value &&
        database->get("legacy", after) && before == after,
        "failed NX GET changed legacy record");
    require(data.getrange(database, "legacy", 16380, 16390, after) &&
        after == value.substr(16380, 11), "legacy selective migration");
    root(database, "legacy");
    long long expiry;
    require(keys.expiry_time(database, "legacy", expiry) && expiry == deadline,
        "migration changed exact TTL");
    bool applied;
    require(data.mset(database, {{"new", "new"}, {"hash", "replace"}}, true,
        applied) && !applied && !database->get("new", after),
        "MSETNX preflight");
    require(data.mset(database, {{"new", "new"}, {"hash", value}}, false,
        applied) && applied && data.get(database, "hash", after) &&
        after == value, "MSET replacement");
    require(data.set(database, "empty", ""), "empty String seed");
    std::string type;
    require(keys.type(database, "empty", type) && type == "string" &&
        data.get(database, "empty", after) && after.empty(),
        "empty String type");
}

static void test_key_operations() {
    auto source = memory(), destination = memory();
    dao::paged_string data;
    dao::json_key keys;
    dao::paged_hash hash;
    const std::string value(200000, 'x');
    require(data.set(source, "key", value) &&
        keys.set_expiry(source, "key", deadline, 0) &&
        hash.hset(destination, "key", "old", "value"), "COPY seed");
    bool copied;
    require(keys.copy(source, "key", destination, "key", true, copied) &&
        copied, "cross-DB String COPY");
    std::string out;
    long long expiry, length;
    require(data.get(destination, "key", out) && out == value &&
        keys.expiry_time(destination, "key", expiry) && expiry == deadline &&
        data.setrange(destination, "key", 0, "different", length) &&
        data.get(source, "key", out) && out == value,
        "COPY bytes/TTL/independence");
    require(keys.rename(source, "key", "renamed", false, copied) && copied &&
        keys.persist(source, "renamed") && data.get(source, "renamed", out) &&
        out == value && keys.expiry_time(source, "renamed", expiry) &&
        expiry == -1, "String RENAME/PERSIST");
    require(data.set(source, "empty", "") && keys.copy(source, "empty",
        destination, "empty", false, copied) && copied &&
        data.get(destination, "empty", out) && out.empty(),
        "COPY empty String");
}

static void test_errors() {
    auto database = memory();
    dao::paged_string data;
    for (const auto& bytes : {std::string("PSR\2future", 10),
        std::string("PSR\1broken", 10)}) {
        require(database->set("bad", bytes), "bad record seed");
        std::string out = "stale", after;
        long long length;
        require(!data.get(database, "bad", out) && out.empty() &&
            !data.append(database, "bad", "x", length) &&
            !data.getrange(database, "bad", 0, -1, out) &&
            database->get("bad", after) && after == bytes,
            "bad/future String overwritten");
    }
    require(data.set(database, "large", std::string(200000, 'x')),
        "large numeric seed");
    long long integer;
    std::string floating;
    require(!data.increment(database, "large", 1, false, integer) &&
        data.status() == dao::dao_base::read_status::invalid_integer &&
        !data.increment_float(database, "large", "1", floating) &&
        data.status() == dao::dao_base::read_status::invalid_score,
        "large numeric validation");
}

static void test_allocation_rollback(bool migration) {
    const std::string initial(40000, 'a'), replacement(60000, 'b');
    bool success = false;
    unsigned failures = 0;
    for (long budget = 0; budget < 1400; ++budget) {
        auto database = memory();
        dao::paged_string data;
        dao::json_string legacy;
        if (migration) require(legacy.set(database, "key", initial),
            "legacy failure seed");
        else require(data.set(database, "key", initial), "native failure seed");
        std::string before, after, out = "stale";
        require(database->get("key", before), "allocation snapshot");
        allocation_budget = budget;
        bool ok = false;
        try {
            if (migration) ok = data.getrange(database, "key", 0, 1, out);
            else {
                dao::set_result changed;
                dao::set_options options; options.return_old = true;
                ok = data.set(database, "key", replacement, options, changed);
            }
        } catch (const std::bad_alloc&) {}
        allocation_budget = -1;
        if (ok) { success = true; root(database, "key"); break; }
        ++failures;
        require(database->get("key", after) && after == before,
            "failed String changed root");
        if (migration)
            require(database->transact_pages({"key"},
                [](page_transaction& tx) {
                std::string page;
                return tx.lookup("key", 1, page) == db::lookup_status::missing;
            }) == db::batch_status::ok, "failed migration left pages");
        require(data.get(database, "key", after) && after == initial,
            "failed String changed pages");
    }
    require(success && failures > 10, "String DAO allocation sweep");
}
static void test_mset_rollback() {
    const std::string initial(40000, 'a'), replacement(60000, 'b');
    const std::map<std::string, std::string> values = {{"a", replacement},
        {"b", replacement}};
    unsigned failures = 0;
    bool success = false;
    for (long budget = 0; budget < 1400; ++budget) {
        auto database = memory();
        dao::paged_string data;
        bool applied;
        require(data.mset(database, {{"a", initial}, {"b", initial}}, false,
            applied) && applied, "MSET rollback seed");
        applied = true;
        allocation_budget = budget;
        bool ok = false;
        try { ok = data.mset(database, values, false, applied); }
        catch (const std::bad_alloc&) {}
        allocation_budget = -1;
        if (ok) {
            require(applied, "MSET applied flag"); success = true; break;
        }
        ++failures;
        std::string a, b;
        require(!applied && data.get(database, "a", a) &&
            data.get(database, "b", b) && a == initial && b == initial,
            "failed MSET published partial pages");
    }
    require(success && failures > 10, "MSET allocation sweep");
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
static void test_locality() {
    auto native = std::make_shared<tracked_db>();
    shared_db database = native;
    require(database->open(""), "tracked open");
    dao::paged_string data;
    dao::json_key keys;
    require(data.set(database, "large", std::string(8 * 1024 * 1024, 'x')),
        "DAO locality seed");
    std::string out;
    long long length;
    native->reads = native->writes = 0;
    allocation_limit = 32768;
    const bool ok = data.getrange(database, "large", 16383, 16384, out) &&
        out == "xx" && data.setrange(database, "large", 5 * 16384 + 1, "y",
        length) && data.append(database, "large", "z", length);
    allocation_limit = SIZE_MAX;
    require(ok && native->reads == 3 && native->writes == 2,
        "String DAO touched/materialized unrelated pages");
    native->reads = native->writes = 0;
    require(keys.set_expiry(database, "large", deadline, 0) &&
        data.length(database, "large", length) && length == 8 * 1024 * 1024 + 1
        && !native->reads && !native->writes,
        "String TTL/STRLEN touched pages");
}
static void suite() {
    test_model(); test_migration_and_mget(); test_key_operations();
    test_errors(); test_allocation_rollback(false);
    test_allocation_rollback(true);
    test_mset_rollback();
}
int main() {
    acl::acl_cpp_init(); test_locality(); suite();
#ifdef HAS_TBB
    use_tbb = true; suite();
#endif
    std::cout << "Native String DAO, migration and key operations passed\n";
}
