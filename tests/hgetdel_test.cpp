#include "stdafx.h"
#include "dao/paged_hash.h"
#include "dao/json/json_hash.h"
#include "dao/json/json_key.h"
#include "db/mdb/mdb.h"
#include "coder/json_record_codec.h"
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <new>
#include <thread>

static thread_local long budget = -1;
static thread_local size_t ceiling = SIZE_MAX;
void* operator new(std::size_t size) {
    if (!budget || size > ceiling) throw std::bad_alloc();
    if (budget > 0) --budget;
    if (void* ptr = std::malloc(size ? size : 1)) return ptr;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* ptr) noexcept { std::free(ptr); }
void operator delete[](void* ptr) noexcept { ::operator delete(ptr); }
#if __cplusplus >= 201402L
void operator delete(void* ptr, std::size_t) noexcept {
    ::operator delete(ptr);
}
void operator delete[](void* ptr, std::size_t) noexcept {
    ::operator delete(ptr);
}
#endif
using namespace pkv;
using values = dao::hash_field_values;
static const int64_t future = 4102444800123LL;
static void require(bool ok, const char* message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}
static shared_db memory() {
    auto db = std::make_shared<mdb>(4);
    require(db->open(""), "open memory");
    return db;
}
static bool take(dao::hash& hash, shared_db db,
    const std::vector<std::string>& fields, values& out,
    const std::string& key = "hash") {
    return hash.hgetdel(db, key, [&](std::vector<std::string>& names) {
        names = fields; return true;
    }, out);
}
static int64_t expiry(shared_db db) {
    dao::json_key keys;
    long long deadline;
    require(keys.expiry_time(db, "hash", deadline), "read deadline");
    return deadline;
}
static void deadline(dao::hash& hash, shared_db db) {
    if (dynamic_cast<dao::json_hash*>(&hash)) {
        // Keep this explicit legacy fixture in JSON: the public expiry DAO
        // deliberately migrates live Hash roots to native pages.
        require(hash.hgetall(db, "hash"), "legacy TTL contents");
        value_record record;
        record.header.kind = value_kind::hash;
        record.header.expires_at_ms = future;
        record.hash_value = hash.get_fields();
        json_record_codec codec; std::string bytes;
        require(codec.encode(record, bytes) == codec_status::ok &&
            db->set("hash", bytes), "legacy TTL fixture");
    } else {
        dao::json_key keys;
        require(keys.set_expiry(db, "hash", future, 0), "set native TTL");
    }
}
static void semantics(dao::hash& hash, shared_db db) {
    values out;
    require(take(hash, db, {"missing", "missing"}, out) &&
        out.size() == 2 && !out[0].found && !out[1].found, "missing hash");
    const std::string binary("f\0x", 3), bytes("v\0\xff", 3);
    require(hash.hmset(db, "hash", {{"", ""}, {binary, bytes},
        {"a", "one"}, {"b", "two"}, {"c", "three"}}) == 5, "seed");
    deadline(hash, db);
    require(take(hash, db, {binary, binary, "absent", "", "a"}, out) &&
        out.size() == 5 && out[0].found && out[0].value == bytes &&
        !out[1].found && !out[2].found && out[3].found &&
        out[3].value.empty() && out[4].value == "one", "ordered duplicates");
    require(expiry(db) == future && hash.hgetall(db, "hash") &&
        hash.get_fields() == std::map<std::string, std::string>(
            {{"b", "two"}, {"c", "three"}}), "remaining data and TTL");
    require(take(hash, db, {"b", "c", "b"}, out) && out[0].found &&
        out[1].found && !out[2].found, "drain");
    std::string root;
    require(!db->get("hash", root), "drained root persists");
}
class tracked_db : public mdb {
public:
    tracked_db() : mdb(4) {}
    bool reject = false, fail_read = false;
    size_t commits = 0, blob_reads = 0;
protected:
    lookup_status dblookup_page(const std::string& owner, uint64_t id,
        std::string& out) override {
        if (fail_read) return lookup_status::error;
        const auto result = mdb::dblookup_page(owner, id, out);
        if (result == lookup_status::found && out.compare(0, 3, "PMB") == 0)
            ++blob_reads;
        return result;
    }
    batch_status dbapply_pages(const std::vector<batch_entry>& roots,
        const std::vector<page_change>& pages) override {
        ++commits;
        for (const auto& page : pages)
            require(page.value.size() <= 16384, "unbounded stored page");
        if (reject) return batch_status::error;
        return mdb::dbapply_pages(roots, pages);
    }
};
static void bounded() {
    auto spy = std::make_shared<tracked_db>(); shared_db db = spy;
    require(db->open(""), "spy open");
    dao::paged_hash hash;
    std::map<std::string, std::string> fields;
    for (unsigned i = 0; i < 64; ++i)
        fields.emplace(std::to_string(i), std::string(50000, 'x'));
    fields.emplace("target", "small");
    require(hash.hmset(db, "hash", fields) == 65, "large hash seed");
    spy->commits = spy->blob_reads = 0;
    values out;
    ceiling = 32768;
    const bool ok = take(hash, db, {"target", "target", "missing"}, out);
    ceiling = SIZE_MAX;
    require(ok && out[0].found && out[0].value == "small" &&
        !out[1].found && !out[2].found && spy->commits == 1 &&
        spy->blob_reads == 0, "read unrelated large field or split commit");
    spy->commits = 0;
    require(take(hash, db, {"missing"}, out) && !spy->commits,
        "missing fields committed");
    spy->reject = true;
    require(!take(hash, db, {"1", "2"}, out) && out.empty(),
        "rejected commit returned values");
    spy->reject = false; spy->fail_read = true;
    require(!take(hash, db, {"1"}, out) && out.empty(), "failed read");
    spy->fail_read = false;
    std::string value;
    require(hash.hget(db, "hash", "1", value) && value == fields["1"] &&
        hash.hget(db, "hash", "2", value) && value == fields["2"],
        "failed operation changed fields");
}
static void allocation(dao::hash& hash) {
    bool success = false; unsigned failures = 0;
    for (long limit = 0; limit < 3000 && !success; ++limit) {
        auto db = memory();
        const std::map<std::string, std::string> seed = {
            {"a", std::string(20000, 'a')}, {"b", "b"}, {"c", "c"},
            {"d", "d"}, {"e", "e"}, {"f", "f"}};
        require(hash.hmset(db, "hash", seed) == 6, "allocation seed");
        deadline(hash, db);
        const dao::hash_fields_parser parse = [](std::vector<std::string>& n) {
            n = {"a", "b", "a"}; return true;
        };
        values out;
        budget = limit;
        try { success = hash.hgetdel(db, "hash", parse, out); }
        catch (const std::bad_alloc&) { success = false; }
        budget = -1;
        if (success) {
            require(out.size() == 3 && out[0].value == seed.at("a") &&
                out[1].value == "b" && !out[2].found, "success result");
            auto remaining = seed; remaining.erase("a"); remaining.erase("b");
            require(hash.hgetall(db, "hash") &&
                hash.get_fields() == remaining && expiry(db) == future,
                "successful take changed unrelated fields or TTL");
        } else {
            ++failures;
            require(out.empty() && hash.hgetall(db, "hash") &&
                hash.get_fields() == seed && expiry(db) == future,
                "allocation failure changed fields or deadline");
        }
    }
    std::cout << "HGETDEL allocation failure points: " << failures <<
        ", reached success: " << success << '\n';
    require(success && failures > 10, "allocation sweep incomplete");
}
static void legacy() {
    auto db = memory(); dao::paged_hash hash; dao::json_hash json;
    require(json.hmset(db, "hash", {{"a", "a"}, {"b", "b"}}) == 2,
        "legacy seed");
    values out; std::string before, after;
    require(db->get("hash", before), "legacy bytes");
    require(!hash.hgetdel(db, "hash", [](std::vector<std::string>&) {
        return false;
    }, out) && db->get("hash", after) && after == before && out.empty(),
        "parser failure imported legacy data");
    require(take(hash, db, {"a", "a"}, out) && out[0].value == "a" &&
        !out[1].found && db->get("hash", after) &&
        after.compare(0, 3, "PMR") == 0, "legacy native migration");
    for (const auto& bytes : {std::string("PMR\3future", 10),
        std::string("{\"T\":\"H\",\"E\":-1,\"D\":{\"a\":3}}")}) {
        require(db->set("hash", bytes) &&
            !take(hash, db, {"a"}, out) && out.empty() &&
            db->get("hash", after) && after == bytes, "corrupt rewrite");
    }
    require(db->set("hash", "{\"T\":\"S\",\"E\":-1,\"D\":\"v\"}"),
        "wrong type seed");
    bool parsed = false;
    require(!hash.hgetdel(db, "hash", [&](std::vector<std::string>&) {
        parsed = true; return false;
    }, out) && !parsed && hash.status() ==
        dao::dao_base::read_status::wrong_type, "type precedence");
}
static void concurrent() {
    auto db = memory(); dao::paged_hash hash;
    require(hash.hmset(db, "hash", {{"a", "one"}, {"b", "two"}}) == 2,
        "concurrent seed");
    std::atomic<unsigned> wins{0}; std::vector<std::thread> threads;
    for (unsigned i = 0; i < 8; ++i) threads.emplace_back([&] {
        dao::paged_hash data; values out;
        require(take(data, db, {"a", "b"}, out), "concurrent take");
        require(out.size() == 2 && out[0].found == out[1].found,
            "partial concurrent result");
        if (out[0].found) ++wins;
    });
    for (auto& thread : threads) thread.join();
    require(wins == 1, "multiple take winners");
}
static void persistent(const std::string& name, const char* path, bool reopen) {
    auto db = name == "rdb" ? db::create_rdb() : name == "wdb" ?
        db::create_wdb() : db::create_mdb_tbb(4);
    require(db && db->open(path), "backend open");
    dao::paged_hash hash; values out;
    const std::string large(70000, 'x');
    if (!reopen) {
        require(hash.hmset(db, "hash", {{"a", large}, {"b", "b"},
            {"c", large}}) == 3, "persistent seed");
        dao::json_key keys;
        require(keys.set_expiry(db, "hash", future, 0), "persistent TTL");
        require(take(hash, db, {"b", "b", "a"}, out) &&
            out[0].value == "b" && !out[1].found &&
            out[2].value == large, "persistent take");
    }
    require(hash.hgetall(db, "hash") && hash.get_fields() ==
        std::map<std::string, std::string>({{"c", large}}) &&
        expiry(db) == future, "reopened data and TTL");
}
int main(int argc, char** argv) {
    if (argc > 2) {
        persistent(argv[1], argv[2], argc > 3); return 0;
    }
    dao::paged_hash hash; dao::json_hash json;
    semantics(hash, memory()); semantics(json, memory());
    bounded(); legacy(); concurrent(); allocation(hash); allocation(json);
    std::cout << "HGETDEL native and legacy storage passed\n";
}
