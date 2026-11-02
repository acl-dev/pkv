#include "common/acl_audit.h"
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <new>
#include <thread>

static thread_local long budget = -1;
void* operator new(std::size_t size) {
    if (!budget) throw std::bad_alloc();
    if (budget > 0) --budget;
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
static void require(bool ok, const char* reason) {
    if (!ok) { std::cerr << reason << '\n'; std::exit(1); }
}
static acl_event event(const std::string& object, long long time = 1000) {
    acl_event value;
    value.reason = "key"; value.context = "toplevel";
    value.username = "alice"; value.object = object;
    value.client_info = "client"; value.updated = time;
    return value;
}
static void grouping() {
    acl_audit audit(3);
    auto a = event(std::string("a\0b", 3));
    require(audit.record(a), "first event");
    a.updated += 60000; a.client_info = "latest";
    require(audit.record(a), "group event");
    auto rows = audit.snapshot(100);
    require(rows.size() == 1 && rows[0].count == 2 && rows[0].id == 0 &&
        rows[0].created == 1000 && rows[0].updated == 61000 &&
        rows[0].client_info == "latest" && rows[0].object == a.object,
        "grouping metadata/binary object");
    auto delayed = a; delayed.updated = 60000;
    require(audit.record(delayed) && audit.snapshot(1)[0].updated == 61000,
        "delayed writer moved timestamp backwards");
    a.updated += 60001;
    require(audit.record(a) && audit.snapshot(100).size() == 2,
        "window boundary");
    a.username = "bob"; require(audit.record(a), "new username");
    a.context = "multi"; require(audit.record(a), "new context");
    rows = audit.snapshot(100);
    require(rows.size() == 3 && rows[0].id == 3 && rows.back().id == 1,
        "bounded eviction/identity");
    audit.clear(); require(audit.snapshot(1).empty(), "reset");
    require(audit.record(a) && audit.snapshot(1)[0].id == 4,
        "reset reused ID");
    audit.limit(0); require(!audit.enabled(), "disabled state");
    budget = 0; const auto ok = audit.record(a); budget = -1;
    require(ok && audit.snapshot(1).empty(), "disabled allocation");
    audit.limit(1); require(audit.record(a), "reenabled");
}
static void recent_ten() {
    acl_audit audit(32);
    for (unsigned i = 0; i < 11; ++i)
        require(audit.record(event(std::to_string(i))), "scan setup");
    require(audit.record(event("1", 1100)), "tenth match");
    auto rows = audit.snapshot(32);
    require(rows.size() == 11 && rows[0].id == 1 && rows[0].count == 2,
        "tenth entry not promoted");
    require(audit.record(event("0", 1100)), "eleventh event");
    rows = audit.snapshot(32);
    require(rows.size() == 12 && rows[0].id == 11 && rows[0].count == 1,
        "scanned more than ten entries");
    audit.limit(2); require(audit.snapshot(32).size() == 2, "limit trim");
    require(audit.snapshot(0).empty(), "zero snapshot count");
}
static void allocation_failures() {
    size_t failed = 0, passed = 0;
    const auto a = event(std::string(512, 'a'));
    auto b = event(std::string(512, 'b'));
    b.username.assign(512, 'u'); b.client_info.assign(1024, 'c');
    for (bool duplicate : {false, true}) for (long count = 0; count < 16;
        ++count) {
        acl_audit audit(1);
        require(audit.record(a), "OOM setup");
        if (duplicate) b.object = a.object;
        else b.object.assign(512, 'b');
        b.username = a.username;
        budget = count; const auto ok = audit.record(b); budget = -1;
        const auto rows = audit.snapshot(10);
        require(rows.size() == 1, "OOM lost retained entry");
        if (ok) ++passed;
        else {
            ++failed;
            require(rows[0].object == a.object && rows[0].count == 1 &&
                rows[0].client_info == a.client_info && audit.dropped() == 1,
                "audit failure changed retained state");
        }
    }
    require(failed && passed, "allocation budget coverage");
}
static void deferred_limits() {
    acl_audit audit;
    for (unsigned i = 0; i < 3; ++i)
        require(audit.record(event(std::to_string(i))), "deferred setup");
    audit.defer_limit(1);
    require(audit.limit() == 1 && audit.snapshot(100).size() == 3,
        "deferred limit eagerly trimmed");
    require(audit.record(event("2")) && audit.snapshot(100).size() == 3,
        "coalescing eagerly trimmed");
    require(audit.record(event("trigger")) && audit.snapshot(100).size() == 1,
        "distinct event did not trim");
    audit.defer_limit(0);
    require(audit.snapshot(100).size() == 1, "disabled eagerly trimmed");
    require(audit.record(event("ignored")) && audit.snapshot(100).empty(),
        "disabled event did not clear");
    audit.defer_limit(2);
    require(audit.record(event("again")) && audit.snapshot(1)[0].id == 4,
        "deferred limits reset ID");
}
static void concurrent() {
    acl_audit audit(128);
    std::atomic<bool> done{false};
    std::vector<std::thread> readers, writers;
    for (unsigned i = 0; i < 4; ++i) readers.emplace_back([&] {
        while (!done.load()) {
            const auto rows = audit.snapshot(128);
            require(rows.size() <= 1 && (rows.empty() ||
                (rows[0].object == "same" && rows[0].id == 0 &&
                 rows[0].count > 0)), "torn audit snapshot");
        }
    });
    for (unsigned i = 0; i < 8; ++i) writers.emplace_back([&] {
        const auto a = event("same");
        for (unsigned n = 0; n < 1000; ++n)
            require(audit.record(a), "concurrent writer");
    });
    for (auto& writer : writers) writer.join();
    done = true;
    for (auto& reader : readers) reader.join();
    require(audit.snapshot(1)[0].count == 8000 && !audit.dropped(),
        "lost concurrent events");
    done = false; writers.clear();
    writers.emplace_back([&] {
        for (unsigned i = 0; i < 1000; ++i) {
            if (i % 2) audit.defer_limit(i % 3);
            else audit.limit(i % 3);
            audit.clear();
        }
        done = true;
    });
    const auto a = event("racing reset");
    while (!done.load()) audit.record(a);
    writers[0].join();
    audit.limit(0); require(audit.snapshot(100).empty(), "racing trim");
}
int main() {
    grouping(); recent_ten(); allocation_failures(); deferred_limits();
    concurrent();
    std::cout << "ACL audit grouping, bounds, IDs, OOM and "
        "concurrency passed\n";
}
