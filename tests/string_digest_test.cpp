#include "stdafx.h"
#include "common/string_digest.h"
#include "dao/paged_string.h"
#include "dao/json/json_string.h"
#include "db/mdb/mdb.h"
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <new>
#include <thread>

static thread_local long budget = -1;
static thread_local size_t largest = SIZE_MAX;
void* operator new(std::size_t size) {
    if (!budget || size > largest) throw std::bad_alloc();
    if (budget > 0) --budget;
    if (void* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { ::operator delete(p); }
#if __cplusplus >= 201402L
void operator delete(void* p, std::size_t) noexcept { ::operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept { ::operator delete(p); }
#endif
using namespace pkv;
static void require(bool ok, const char* message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}
static shared_db memory(bool tbb) {
    auto db = tbb ? db::create_mdb_tbb(16) : db::create_mdb(16);
    require(db && db->open(""), "digest DB open"); return db;
}
static std::string bytes(size_t length) {
    std::string out(length, '\0');
    for (size_t i = 0; i < length; ++i) out[i] = static_cast<char>(i % 251);
    return out;
}
static void vectors() {
    require(string_digest::value("") == "2d06800538d394c2" &&
        string_digest::value("Hello world") == "b6acb9d84a38ff74",
        "XXH3 known vectors");
    for (size_t length : {0, 1, 3, 4, 8, 9, 16, 17, 32, 64, 128, 129,
        240, 241, 1023, 1024, 1025, 16383, 16384, 16385, 70000}) {
        const auto value = bytes(length);
        for (size_t stride : {1, 3, 17, 241, 16384}) {
            string_digest stream;
            for (size_t i = 0; i < length; i += stride)
                stream.update(value.substr(i, stride));
            require(stream.finish() == string_digest::value(value),
                "XXH3 streaming boundary mismatch");
        }
    }
    require(string_digest::equal("B6ACB9D84A38FF74",
        string_digest::value("Hello world")) &&
        !string_digest::equal("gggggggggggggggg", "0000000000000000") &&
        !string_digest::equal("short", "0000000000000000"),
        "digest comparison encoding");
}
class tracked_db : public mdb {
public:
    tracked_db() : mdb(4) {}
    size_t reads = 0;
    bool broken = false, reject = false;
protected:
    lookup_status dblookup_page(const std::string& key, uint64_t id,
        std::string& out) override {
        ++reads;
        if (broken && id == 2) return lookup_status::missing;
        return mdb::dblookup_page(key, id, out);
    }
    batch_status dbapply_pages(const std::vector<batch_entry>& roots,
        const std::vector<page_change>& pages) override {
        if (reject) return batch_status::error;
        return mdb::dbapply_pages(roots, pages);
    }
};
static void locality() {
    auto spy = std::make_shared<tracked_db>();
    shared_db db = spy;
    require(db->open(""), "digest spy open");
    dao::paged_string data;
    const auto value = bytes(4 * 1024 * 1024);
    const auto hash = string_digest::value(value);
    require(data.set(db, "key", value), "digest large seed");
    dao::string_result digest;
    largest = 65536;
    const bool ok = data.digest(db, "key", digest);
    largest = SIZE_MAX;
    require(ok && digest.found && digest.value == hash && spy->reads == 256,
        "DIGEST materialized or misread native pages");
    dao::set_options options;
    options.when = dao::set_options::digest_different;
    options.comparison = &hash;
    dao::set_result out;
    spy->reads = 0; largest = 65536;
    const bool skipped = data.set(db, "key", "bad", options, out);
    largest = SIZE_MAX;
    require(skipped && !out.applied && spy->reads == 256,
        "digest SET materialized comparison input");
    options.return_old = true; spy->reads = 0;
    require(data.set(db, "key", "bad", options, out) && !out.applied &&
        out.old_value == value && spy->reads == 256,
        "digest GET reread payload pages");
    options.when = dao::set_options::digest_equal;
    options.return_old = false;
    spy->broken = true;
    require(!data.digest(db, "key", digest) && !digest.found &&
        digest.value.empty(), "corrupt DIGEST returned partial result");
    long long removed;
    require(!data.delex(db, "key", options, removed) && !removed,
        "DELEX ignored missing comparison page");
    spy->broken = false; spy->reject = true;
    require(!data.delex(db, "key", options, removed) && !removed,
        "rejected DELEX reported deletion");
    spy->reject = false;
    require(data.digest(db, "key", digest) && digest.value == hash,
        "failed DELEX changed payload");
    largest = 65536;
    const bool deleted = data.delex(db, "key", options, removed);
    largest = SIZE_MAX;
    require(deleted && removed == 1, "DELEX large delete");
    require(db->transact_pages({"key"}, [](page_transaction& tx) {
        std::string page;
        return tx.lookup("key", 1, page) == db::lookup_status::missing;
    }) == db::batch_status::ok, "DELEX retained owned pages");
}
static void legacy(bool tbb) {
    auto db = memory(tbb);
    dao::json_string json;
    dao::paged_string data;
    const auto value = bytes(70000), hash = string_digest::value(value);
    std::string before, after;
    require(json.set(db, "key", value) && db->get("key", before),
        "digest legacy seed");
    dao::string_result out;
    require(data.digest(db, "key", out) && out.found && out.value == hash &&
        db->get("key", after) && before == after,
        "DIGEST migrated read-only legacy bytes");
    dao::set_options options;
    options.when = dao::set_options::digest_different;
    options.comparison = &hash;
    long long removed;
    require(data.delex(db, "key", options, removed) && !removed &&
        db->get("key", after) && before == after, "DELEX legacy no-op");
    options.when = dao::set_options::digest_equal;
    require(data.delex(db, "key", options, removed) && removed == 1,
        "DELEX legacy remove");
    require(json.set(db, "key", value) && json.digest(db, "key", out) &&
        out.value == hash && json.delex(db, "key", options, removed) &&
        removed == 1, "legacy JSON digest/delete");
}
static void race(bool tbb) {
    auto db = memory(tbb);
    dao::paged_string data;
    const std::string value(40000, 'x'), hash = string_digest::value(value);
    require(data.set(db, "key", value), "DELEX race seed");
    std::atomic<unsigned> winners(0);
    std::vector<std::thread> workers;
    for (unsigned i = 0; i < 8; ++i) workers.emplace_back([&] {
        dao::paged_string local;
        dao::set_options options;
        options.when = dao::set_options::digest_equal;
        options.comparison = &hash;
        long long removed;
        require(local.delex(db, "key", options, removed), "DELEX race error");
        winners += static_cast<unsigned>(removed);
    });
    for (auto& worker : workers) worker.join();
    require(winners == 1, "DELEX race had multiple winners");
}
static void rollback(bool tbb) {
    const auto value = bytes(40000), hash = string_digest::value(value);
    for (bool deleting : {false, true}) {
        unsigned failures = 0;
        bool success = false;
        for (long limit = 0; limit < 500; ++limit) {
            auto db = memory(tbb);
            dao::paged_string data;
            require(data.set(db, "key", value), "digest rollback seed");
            dao::set_options options;
            options.when = dao::set_options::digest_equal;
            options.comparison = &hash;
            dao::string_result out;
            long long removed = 0;
            budget = limit;
            bool ok = false;
            try { ok = deleting ? data.delex(db, "key", options, removed) :
                data.digest(db, "key", out); }
            catch (const std::bad_alloc&) {}
            budget = -1;
            if (ok) {
                require(deleting ? removed == 1 : out.value == hash,
                    "digest allocation success result");
                success = true; break;
            }
            ++failures;
            std::string actual;
            require(!removed && !out.found && data.get(db, "key", actual)
                && actual == value, "failed digest operation changed value");
        }
        require(success && failures > 10, "digest allocation sweep incomplete");
    }
}
int main() {
    acl::acl_cpp_init(); vectors(); locality();
    for (bool tbb : {false, true}) {
#ifndef HAS_TBB
        if (tbb) continue;
#endif
        legacy(tbb); race(tbb); rollback(tbb);
    }
    std::cout << "DIGEST/DELEX streaming and rollback checks passed\n";
}
