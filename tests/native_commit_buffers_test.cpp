#include "stdafx.h"
#include "db/mdb/mdb.h"
#include "db/page_transaction.h"
#include "db/mdb/replacement_workspace.h"
#include "coder/list_page.h"
#include <functional>
#include <cstdlib>
#include <iostream>
#include <new>
static thread_local bool measuring = false;
static thread_local size_t allocations = 0;
static thread_local int fail_after = -1;
// Observe live large new allocations without changing allocator alignment or
// adding headers. Only the private, single-threaded root regression enables it.
static thread_local bool track_large = false, tracking_overflow = false;
static thread_local void* large_blocks[2048]{};
static thread_local size_t large_sizes[2048]{}, live_large = 0;
static void track_block(void* p, size_t bytes) {
    if (!track_large || bytes < 2048 || bytes > 8192) return;
    for (size_t i = 0; i < 2048; ++i) if (!large_blocks[i]) {
        large_blocks[i] = p; large_sizes[i] = bytes; live_large += bytes;
        return;
    }
    tracking_overflow = true;
}
static void release_block(void* p) {
    if (!track_large || !p) return;
    for (size_t i = 0; i < 2048; ++i) if (large_blocks[i] == p) {
        live_large -= large_sizes[i]; large_blocks[i] = nullptr;
        return;
    }
}
void* operator new(std::size_t bytes) {
    if (fail_after == 0) throw std::bad_alloc();
    if (fail_after > 0) --fail_after;
    if (measuring) ++allocations;
    if (void* p = std::malloc(bytes ? bytes : 1)) {
        track_block(p, bytes); return p;
    }
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { release_block(p); std::free(p); }
void operator delete[](void* p) noexcept { ::operator delete(p); }
#if __cplusplus >= 201402L
void operator delete(void* p, std::size_t) noexcept { ::operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept { ::operator delete(p); }
#endif
// Private engine, no competing actor; count native commit allocations only.
class native_db : public pkv::mdb {
public:
    native_db() : mdb(4) {}
    batch_status apply(const std::vector<batch_entry>& roots,
        const std::vector<pkv::page_change>& pages = {}) {
        return dbapply_pages(roots, pages);
    }
    batch_status prepare(const std::vector<batch_entry>& roots,
        prepare_callback& callback) {
        return dbprepare_pages(roots, {}, callback);
    }
    batch_status owned(const std::vector<batch_entry>& roots,
        std::unique_ptr<prepared_batch>& out) {
        static const std::vector<pkv::page_change> empty;
        return dbprepare_owned_pages(roots, empty, out);
    }
    batch_status prepare_pages(const std::vector<batch_entry>& roots,
        const std::vector<pkv::page_change>& pages,
        prepare_callback& callback) {
        return dbprepare_pages(roots, pages, callback);
    }
    batch_status owned_pages(const std::vector<batch_entry>& roots,
        const std::vector<pkv::page_change>& pages,
        std::unique_ptr<prepared_batch>& out) {
        return dbprepare_owned_pages(roots, pages, out);
    }
    bool page(const std::string& key, uint64_t id, std::string& out) {
        return dblookup_page(key, id, out) == lookup_status::found;
    }
};
static void require(bool ok, const char* reason) {
    if (!ok) { std::cerr << reason << '\n'; std::exit(1); }
}
static void test_spill_after_exact_shrink() {
    using namespace pkv;
    native_db db;
    require(db.open(""), "mixed spill open");
    const std::string key = "mixed-spill";
    std::vector<db::batch_entry> roots;
    roots.emplace_back(key, false, "root");
    roots[0].action = db::update_action::write;
    std::vector<page_change> pages;
    for (uint64_t id = 1; id <= 3; ++id)
        pages.emplace_back(key, id, std::string(128, 'a'),
            db::update_action::write);
    require(db.apply(roots, pages) == db::batch_status::ok, "mixed spill seed");
    roots.clear();
    roots.emplace_back(key, true, "root");
    roots[0].action = db::update_action::write;
    // Equal-size root and page 1 use the two inline slots. Shrinking page 2
    // stages an exact copy without using spill slot zero. Page 3 grows.
    pages[1].value.assign(64, 'b');
    pages[2].value.assign(192, 'c');
    require(db.apply(roots, pages) == db::batch_status::ok &&
        detail::replacement_workspace::retained_spill_entries() == 2,
        "mixed spill must construct both cache slots");
    for (const auto& page : pages) {
        std::string bytes;
        require(db.page(key, page.id, bytes) && bytes == page.value,
            "mixed spill published wrong bytes");
    }
}
static void test_page_commit() {
    native_db db;
    require(db.open(""), "page engine open");
    std::vector<pkv::db::batch_entry> roots;
    roots.emplace_back("key", false, std::string(128, 'a'));
    roots[0].action = pkv::db::update_action::write;
    std::vector<pkv::page_change> pages;
    pages.emplace_back("key", 1, std::string(128, 'p'),
        pkv::db::update_action::write);
    require(db.apply(roots, pages) == pkv::db::batch_status::ok, "page seed");
    roots.clear();
    roots.emplace_back("key", true, std::string(128, 'b'));
    roots[0].action = pkv::db::update_action::write;
    pages[0].value.assign(128, 'q');
    require(db.apply(roots, pages) == pkv::db::batch_status::ok,
        "page replacement warmup");
    roots[0].value.assign(128, 'c'); pages[0].value.assign(128, 'r');
    allocations = 0; measuring = true;
    const auto result = db.apply(roots, pages);
    measuring = false;
    require(result == pkv::db::batch_status::ok && allocations == 0,
        "single-owner page commit buffers allocated");
    std::string root, page;
    require(db.get("key", root) && root == std::string(128, 'c') &&
        db.page("key", 1, page) && page == std::string(128, 'r'),
        "page commit bytes");
    std::cout << "root and page overwrite: " << allocations << '\n';
}

class capacity_db : public native_db {
public:
    size_t page_capacity(const std::string& key, uint64_t id) {
        size_t capacity = 0;
        std::string scratch;
        page_inspection inspection([](void* raw, const std::string& bytes) {
            *static_cast<size_t*>(raw) = bytes.capacity();
        }, &capacity);
        const auto status = dbinspect_page(key, id, scratch, inspection);
        require(status == lookup_status::found && inspection.matches(status),
            "native page capacity lookup");
        return capacity;
    }
protected:
    bool dbborrows_pages() const override { return true; }
};

static void test_replacement_failure() {
    native_db db;
    require(db.open(""), "replacement failure engine");
    std::vector<pkv::db::batch_entry> roots;
    roots.emplace_back("failure", false, "original-root");
    roots[0].action = pkv::db::update_action::write;
    std::vector<pkv::page_change> pages;
    pages.emplace_back("failure", 7, "original-page",
        pkv::db::update_action::write);
    require(db.apply(roots, pages) == pkv::db::batch_status::ok,
        "replacement failure seed");
    roots[0].value.assign(65536, 'r');
    pages[0].value.assign(65536, 'p');
    for (int failure : {0, 1}) {
        fail_after = failure;
        const auto status = db.apply(roots, pages);
        fail_after = -1;
        std::string root, page;
        require(status == pkv::db::batch_status::error &&
            db.get("failure", root) && root == "original-root" &&
            db.page("failure", 7, page) && page == "original-page",
            "partial root/page publication on allocation failure");
        require(roots[0].value == std::string(65536, 'r') &&
            pages[0].value == std::string(65536, 'p'),
            "replacement consumed staged input");
    }
    roots[0].value = "modified-root";
    fail_after = 0;
    const auto equal_root = db.apply(roots, pages);
    fail_after = -1;
    std::string root, page;
    require(equal_root == pkv::db::batch_status::error &&
        db.get("failure", root) && root == "original-root" &&
        db.page("failure", 7, page) && page == "original-page",
        "equal-size root published before failed page allocation");
    require(db.apply(roots, pages) == pkv::db::batch_status::ok,
        "replacement recovery");
}

static void test_sealed_list_capacity() {
    using namespace pkv;
    capacity_db db;
    require(db.open(""), "sealed list engine open");
    list_page endpoint;
    endpoint.id = 1;
    list_fragment member;
    member.total_bytes = 16;
    member.bytes.assign(16, 'x');
    endpoint.fragments.assign(51, member);
    std::string bytes;
    require(list_page_codec::encode_packed_page(endpoint, bytes) ==
        list_page_status::ok, "encode list endpoint");
    std::vector<db::batch_entry> roots;
    roots.emplace_back("list", false, "root");
    roots[0].action = db::update_action::write;
    std::vector<page_change> pages;
    pages.emplace_back("list", 1, bytes, db::update_action::write);
    require(db.apply(roots, pages) == db::batch_status::ok, "seed list page");
    roots.clear();
    roots.emplace_back("list", true, "root");
    roots[0].action = db::update_action::write;
    {
        detail::replacement_workspace workspace(true);
        workspace.take(1).second.reserve(2048);
    }
    endpoint.next = 2;
    require(list_page_codec::encode_packed_page(endpoint, pages[0].value) ==
        list_page_status::ok, "encode sealed list page");
    list_page next;
    next.id = 2;
    next.previous = 1;
    next.fragments.push_back(member);
    require(list_page_codec::encode_packed_page(next, bytes) ==
        list_page_status::ok, "encode new list endpoint");
    pages.emplace_back("list", 2, bytes, db::update_action::write);
    const std::string fresh(pages[0].value);
    require(db.apply(roots, pages) == db::batch_status::ok &&
        db.page_capacity("list", 1) <= fresh.capacity() &&
        db.page("list", 1, bytes) && bytes == fresh,
        "sealed list page inherited oversized workspace capacity");
}

static void test_large_growth_capacity() {
    capacity_db db;
    require(db.open(""), "capacity engine open");
    std::vector<pkv::db::batch_entry> roots;
    std::vector<pkv::page_change> pages;
    pages.emplace_back("capacity", 1, "", pkv::db::update_action::write);
    bool found = false;
    for (const size_t size : {2084U, 4136U, 6188U, 8240U, 8240U}) {
        roots.clear();
        roots.emplace_back("capacity", found, std::string(48, 'r'));
        roots[0].action = pkv::db::update_action::write;
        pages[0].value.assign(size, static_cast<char>(size));
        const std::string fresh(pages[0].value);
        require(db.apply(roots, pages) == pkv::db::batch_status::ok,
            "capacity growth commit");
        found = true;
        require(db.page_capacity("capacity", 1) <= fresh.capacity(),
            "large page retained geometric workspace growth");
        std::string value;
        require(db.page("capacity", 1, value) && value == pages[0].value,
            "capacity growth changed page bytes");
    }
}

class ready_call : public pkv::db::prepare_callback {
public:
    std::function<pkv::db::batch_status(pkv::db::prepared_batch&)> run;
    pkv::db::batch_status prepared(pkv::db::prepared_batch& batch) override {
        return run(batch);
    }
};
static void test_prepared_lifetime() {
    for (bool publish : {false, true}) {
        native_db outer, peer;
        require(outer.open("") && peer.open("") &&
            outer.set("key", std::string(128, 'a')) &&
            peer.set("key", std::string(128, 'b')), "prepare seed");
        std::vector<pkv::db::batch_entry> roots, other;
        roots.emplace_back("key", true, std::string(128, 'o'));
        other.emplace_back("key", true, std::string(128, 'p'));
        roots[0].action = other[0].action = pkv::db::update_action::write;
        bool entered = false, finished = false;
        ready_call callback;
        callback.run = [&](pkv::db::prepared_batch& batch) {
            require(peer.apply(other) == pkv::db::batch_status::ok,
                "nested prepared buffers");
            entered = true;
            while (!finished) acl::fiber::yield();
            if (publish) batch.publish();
            return publish ? pkv::db::batch_status::ok :
                pkv::db::batch_status::aborted;
        };
        go[&] { require(outer.prepare(roots, callback) == (publish ?
            pkv::db::batch_status::ok : pkv::db::batch_status::aborted),
            "prepared yield result"); };
        go[&] {
            while (!entered) acl::fiber::yield();
            other[0].value.assign(128, 'q');
            require(peer.apply(other) == pkv::db::batch_status::ok,
                "peer prepared buffers");
            finished = true;
        };
        acl::fiber::schedule();
        std::string value;
        require(outer.get("key", value) && value ==
            std::string(128, publish ? 'o' : 'a') &&
            peer.get("key", value) && value == std::string(128, 'q'),
            "prepared lifetime changed bytes/rollback");
        std::unique_ptr<pkv::db::prepared_batch> owned;
        require(outer.owned(roots, owned) == pkv::db::batch_status::ok &&
            peer.apply(other) == pkv::db::batch_status::ok,
            "owned prepare isolation");
        if (publish) owned->publish();
        owned.reset();
        require(outer.get("key", value) && value ==
            std::string(128, publish ? 'o' : 'a'), "owned rollback bytes");
    }
}
static void test_retained_capacity() {
    native_db db;
    require(db.open("") && db.set("large", std::string(65536, 'a')),
        "large replacement seed");
    std::vector<pkv::db::batch_entry> roots;
    roots.emplace_back("large", true, std::string(65536, 'b'));
    roots[0].action = pkv::db::update_action::write;
    require(db.apply(roots) == pkv::db::batch_status::ok &&
        pkv::detail::replacement_workspace::retained_value_capacity() <=
            2 * pkv::page_transaction::max_value_bytes,
        "replacement cache retained oversized buffers");
    std::string value;
    require(db.get("large", value) && value == std::string(65536, 'b'),
        "large replacement bytes");
}

static void test_spill_commit() {
    native_db db;
    require(db.open(""), "spill open");
    std::vector<pkv::db::batch_entry> roots;
    roots.emplace_back("spill", false, std::string(128, 'a'));
    roots[0].action = pkv::db::update_action::write;
    std::vector<pkv::page_change> pages;
    for (uint64_t id = 1; id <= 4; ++id)
        pages.emplace_back("spill", id, std::string(128, 'a'),
            pkv::db::update_action::write);
    for (unsigned i = 0; i < 3; ++i)
        require(db.apply(roots, pages) == pkv::db::batch_status::ok,
            "spill warmup");
    roots[0].value.assign(128, 'b');
    for (auto& page : pages) page.value.assign(128, 'b');
    allocations = 0; measuring = true;
    const auto status = db.apply(roots, pages);
    measuring = false;
    require(status == pkv::db::batch_status::ok && allocations == 0,
        "equal-length multi-page commit allocated");
    for (uint64_t id = 1; id <= 4; ++id) {
        std::string out;
        require(db.page("spill", id, out) && out == std::string(128, 'b'),
            "spill published wrong bytes");
    }
    pages.erase(pages.begin() + 2, pages.end());
    for (auto& page : pages) page.value.assign(128, 'c');
    require(db.apply(roots, pages) == pkv::db::batch_status::ok,
        "smaller spill commit");
    std::string out;
    require(db.page("spill", 4, out) && out == std::string(128, 'b'),
        "unused cached target was published");
}

static void test_spill_failure() {
    native_db db;
    require(db.open(""), "spill failure open");
    std::vector<pkv::db::batch_entry> roots;
    roots.emplace_back("fault", false, std::string(128, 'a'));
    roots[0].action = pkv::db::update_action::write;
    std::vector<pkv::page_change> pages;
    for (uint64_t id = 1; id <= 4; ++id)
        pages.emplace_back("fault", id, std::string(128, 'a'),
            pkv::db::update_action::write);
    require(db.apply(roots, pages) == pkv::db::batch_status::ok,
        "spill failure seed");
    roots[0].value.assign(65536, 'r');
    for (auto& page : pages) page.value.assign(65536, 'p');
    bool completed = false;
    for (int failure = 0; failure < 32; ++failure) {
        fail_after = failure;
        const auto status = db.apply(roots, pages);
        fail_after = -1;
        std::string value;
        if (status == pkv::db::batch_status::ok) {
            require(failure > 0 && db.get("fault", value) &&
                value == roots[0].value, "spill success root");
            for (const auto& page : pages)
                require(db.page("fault", page.id, value) && value == page.value,
                    "spill success page");
            completed = true;
            break;
        }
        require(status == pkv::db::batch_status::error &&
            db.get("fault", value) && value == std::string(128, 'a'),
            "failed spill published root");
        for (uint64_t id = 1; id <= 4; ++id)
            require(db.page("fault", id, value) &&
                value == std::string(128, 'a'), "failed spill published page");
        require(roots[0].value == std::string(65536, 'r') &&
            pages.back().value == std::string(65536, 'p'),
            "failed spill consumed input");
    }
    require(completed, "spill fault sweep never completed");
    // Restore the small values before exercising a mixed direct/copy failure.
    roots[0].value.assign(128, 'a');
    for (auto& page : pages) page.value.assign(128, 'a');
    require(db.apply(roots, pages) == pkv::db::batch_status::ok,
        "spill failure restore");
    for (auto& page : pages) page.value.assign(65536, 'p');
    roots[0].value.assign(128, 'r');
    pages[0].value.assign(128, 'p');
    fail_after = 0;
    const auto status = db.apply(roots, pages);
    fail_after = -1;
    std::string value;
    require(status == pkv::db::batch_status::error &&
        db.get("fault", value) && value == std::string(128, 'a') &&
        db.page("fault", 1, value) && value == std::string(128, 'a'),
        "failed resized page published an equal-length root or page");
}

static void test_spill_lifetime() {
    for (bool publish : {false, true}) {
        native_db outer, peer;
        require(outer.open("") && peer.open(""), "spill lifetime open");
        std::vector<pkv::db::batch_entry> roots, other;
        roots.emplace_back("key", false, std::string(128, 'a'));
        other.emplace_back("key", false, std::string(128, 'p'));
        roots[0].action = other[0].action = pkv::db::update_action::write;
        std::vector<pkv::page_change> pages, peers;
        for (uint64_t id = 1; id <= 4; ++id) {
            pages.emplace_back("key", id, std::string(128, 'a'),
                pkv::db::update_action::write);
            peers.emplace_back("key", id, std::string(128, 'p'),
                pkv::db::update_action::write);
        }
        require(outer.apply(roots, pages) == pkv::db::batch_status::ok &&
            peer.apply(other, peers) == pkv::db::batch_status::ok,
            "spill lifetime seed");
        roots[0].value.assign(128, 'b');
        for (auto& page : pages) page.value.assign(128, 'b');
        ready_call callback;
        bool entered = false, finished = false;
        callback.run = [&](pkv::db::prepared_batch& batch) {
            entered = true;
            while (!finished) acl::fiber::yield();
            if (publish) batch.publish();
            return publish ? pkv::db::batch_status::ok :
                pkv::db::batch_status::aborted;
        };
        go[&] { require(outer.prepare_pages(roots, pages, callback) ==
            (publish ? pkv::db::batch_status::ok :
                pkv::db::batch_status::aborted), "spill prepared result"); };
        go[&] {
            while (!entered) acl::fiber::yield();
            for (auto& page : peers) page.value.assign(128, 'q');
            require(peer.apply(other, peers) == pkv::db::batch_status::ok,
                "nested spill commit");
            finished = true;
        };
        acl::fiber::schedule();
        std::string value;
        for (uint64_t id = 1; id <= 4; ++id)
            require(outer.page("key", id, value) && value ==
                std::string(128, publish ? 'b' : 'a'), "spill fiber alias");
        std::unique_ptr<pkv::db::prepared_batch> owned;
        require(outer.owned_pages(roots, pages, owned) ==
            pkv::db::batch_status::ok && peer.apply(other, peers) ==
                pkv::db::batch_status::ok, "owned spill isolation");
        if (publish) owned->publish();
        owned.reset();
        require(outer.get("key", value) && value ==
            std::string(128, publish ? 'b' : 'a'), "owned spill root");
        for (uint64_t id = 1; id <= 4; ++id) {
            require(outer.page("key", id, value) && value ==
                std::string(128, publish ? 'b' : 'a'), "owned spill page");
            require(peer.page("key", id, value) &&
                value == std::string(128, 'q'), "nested spill bytes");
        }
    }
    using spill = pkv::detail::replacement_workspace;
    require(spill::retained_spill_entries() <= spill::max_spill_entries &&
        spill::retained_spill_capacity() <=
            spill::max_spill_entries * spill::max_spill_capacity,
        "spill cache exceeded bounds");
}

static void test_spill_shrink() {
    capacity_db db;
    require(db.open(""), "spill shrink open");
    std::vector<pkv::db::batch_entry> roots;
    roots.emplace_back("shrink", false, std::string(128, 'a'));
    roots[0].action = pkv::db::update_action::write;
    std::vector<pkv::page_change> pages;
    for (uint64_t id = 1; id <= 4; ++id)
        pages.emplace_back("shrink", id, std::string(700, 'p'),
            pkv::db::update_action::write);
    for (unsigned i = 0; i < 3; ++i)
        require(db.apply(roots, pages) == pkv::db::batch_status::ok,
            "spill shrink warmup");
    for (auto& page : pages) page.value.assign(100, 's');
    require(db.apply(roots, pages) == pkv::db::batch_status::ok,
        "spill shrink commit");
    const std::string fresh(100, 's');
    for (uint64_t id = 2; id <= 4; ++id)
        require(db.page_capacity("shrink", id) <= fresh.capacity(),
            "spill published oversized cached capacity");
}

static void test_spill_bounds() {
    native_db db;
    require(db.open(""), "spill bounds open");
    std::vector<pkv::db::batch_entry> roots;
    roots.emplace_back("bounds", false, std::string(128, 'a'));
    roots[0].action = pkv::db::update_action::write;
    std::vector<pkv::page_change> pages;
    for (uint64_t id = 1; id <= 80; ++id)
        pages.emplace_back("bounds", id, std::string(2048, 'p'),
            pkv::db::update_action::write);
    for (unsigned i = 0; i < 3; ++i)
        require(db.apply(roots, pages) == pkv::db::batch_status::ok,
            "oversized spill commit");
    using spill = pkv::detail::replacement_workspace;
    require(spill::retained_spill_entries() <= spill::max_spill_entries &&
        spill::retained_spill_capacity() <=
            spill::max_spill_entries * spill::max_spill_capacity,
        "oversized commit retained spill buffers");
    std::string value;
    require(db.page("bounds", 80, value) && value == std::string(2048, 'p'),
        "oversized spill bytes");
}

static void test_root_only_commit(bool equal_clear) {
    native_db db;
    require(db.open(""), "root-only engine");
    const std::string key("root\0key", 8);
    std::vector<pkv::db::batch_entry> roots;
    roots.emplace_back(key, false, std::string(128, 'a'));
    roots[0].action = pkv::db::update_action::write;
    std::vector<pkv::page_change> pages;
    for (unsigned id = 1; id <= 512; ++id)
        pages.emplace_back(key, id, std::string(1024, char(id)),
            pkv::db::update_action::write);
    require(db.apply(roots, pages) == pkv::db::batch_status::ok,
        "root-only family seed");
    roots[0].value.assign(128, 'b');
    for (unsigned warm = 0; warm < 2; ++warm)
        require(db.apply(roots) == pkv::db::batch_status::ok, "root warmup");
    roots[0].value.assign(128, 'c');
    allocations = 0; measuring = true;
    const auto status = db.apply(roots);
    measuring = false;
    require(status == pkv::db::batch_status::ok && allocations == 0,
        "warm root-only commit allocated");
    std::string value;
    require(db.get(key, value) && value == roots[0].value,
        "root-only published value");
    for (const auto& page : pages)
        require(db.page(key, page.id, value) && value == page.value,
            "root-only overwrite lost preserved pages");
    roots[0].value.assign(65536, 'd');
    roots[0].clear_pages = true;
    fail_after = 0;
    const auto failed = db.apply(roots);
    fail_after = -1;
    require(failed == pkv::db::batch_status::error && db.get(key, value) &&
        value == std::string(128, 'c') && roots[0].value ==
            std::string(65536, 'd'), "root-only failure changed root/input");
    for (const auto& page : pages)
        require(db.page(key, page.id, value) && value == page.value,
            "root-only failure cleared old pages");
    if (equal_clear) roots[0].value.assign(128, 'e');
    require(db.apply(roots) == pkv::db::batch_status::ok &&
        db.get(key, value) && value == roots[0].value,
        "root-only clear publication");
    for (const auto& page : pages)
        require(!db.page(key, page.id, value), "root-only clear retained page");
    roots[0].clear_pages = false;
    roots[0].value.clear();
    require(db.apply(roots) == pkv::db::batch_status::ok &&
        db.get(key, value) && value.empty(), "empty root-only value");
    roots.clear();
    roots.emplace_back("missing", true, "new-root");
    roots[0].action = pkv::db::update_action::write;
    roots[0].clear_pages = true;
    uint64_t count = 0;
    require(db.apply(roots) == pkv::db::batch_status::ok &&
        db.get("missing", value) && value == "new-root" &&
        db.key_count(count) == pkv::db::count_status::ok && count == 2,
        "missing root fallback lost key index");
}

static void test_adopt_family_rollback(bool multiple_roots) {
    using database = pkv::db;
    pkv::mdb db(4);
    const std::string key = "adopt-family";
    const std::string payload(36, 'p');
    require(db.open(""), "adopt family engine open");
    const database::page_callback replace = [&](pkv::page_transaction& tx) {
        auto* root = tx.roots().find(key);
        root->value = "replacement";
        root->action = database::update_action::write;
        if (!tx.reset_pages(key)) return false;
        for (uint64_t id = 1; id <= 48; ++id) {
            std::string bytes(payload);
            if (id == 48) bytes.reserve(1024);
            if (!tx.put(key, id, std::move(bytes))) return false;
        }
        return true;
    };
    require(db.transact_page(key, [&](pkv::page_transaction& tx) {
        auto* root = tx.roots().find(key);
        root->value = "original";
        root->action = database::update_action::write;
        return tx.put(key, 99, "old page");
    }) == database::batch_status::ok, "adopt family seed");
    bool committed = false;
    for (int budget = 0; budget < 400 && !committed; ++budget) {
        std::vector<std::string> keys{key, "read-only"};
        fail_after = budget;
        const auto status = multiple_roots ?
            db.transact_pages(std::move(keys), replace) :
            db.transact_page(key, replace);
        fail_after = -1;
        committed = status == database::batch_status::ok;
        require(committed || status == database::batch_status::error,
            "unexpected adopt family status");
        require(db.transact_page(key, [&](pkv::page_transaction& tx) {
            const auto* root = tx.roots().find(key);
            std::string bytes;
            require(root->value == (committed ? "replacement" : "original"),
                "adopt family root publication");
            require(tx.lookup(key, 99, bytes) == (committed ?
                database::lookup_status::missing : database::lookup_status::found),
                "adopt family old page publication");
            if (!committed) require(bytes == "old page", "adopt family rollback bytes");
            for (uint64_t id = 1; id <= 48; ++id) {
                require(tx.lookup(key, id, bytes) == (committed ?
                    database::lookup_status::found : database::lookup_status::missing),
                    "adopt family page publication");
                if (committed) require(bytes == payload, "adopt family new page bytes");
            }
            return true;
        }) == database::batch_status::ok, "inspect adopted family");
    }
    require(committed, "adopt family fault sweep incomplete");
}

static void test_small_root_after_large_collection() {
    using namespace pkv;
    native_db db;
    require(db.open(""), "root shrink open");
    const std::string large(4096, 'x');
    std::vector<std::string> keys;
    for (unsigned i = 0; i < 256; ++i) keys.push_back("root-" + std::to_string(i));
    track_large = true;
    for (const auto& key : keys) require(db.set(key, large), "large root seed");
    const auto before = live_large;
    std::vector<db::batch_entry> roots;
    for (const auto& key : keys) {
        roots.clear(); roots.emplace_back(key, true, "metadata");
        roots[0].action = db::update_action::write;
        require(db.apply(roots) == db::batch_status::ok, "small root publish");
    }
    const auto after = live_large;
    track_large = false;
    require(!tracking_overflow && before >= after + 128 * 4096,
        "small roots retained prior collection allocations");
    for (const auto& key : keys) {
        std::string out;
        require(db.get(key, out) && out == "metadata", "small root contents");
    }
}

int main() {
    acl::acl_cpp_init();
    test_small_root_after_large_collection();
    test_spill_after_exact_shrink();
    test_replacement_failure();
    for (bool erase : {false, true}) {
        native_db db;
        if (!db.open("") || !db.set("key", std::string(128, 'a'))) return 1;
        std::vector<pkv::db::batch_entry> roots;
        roots.emplace_back("key", true, std::string(128, 'b'));
        roots[0].action = erase ? pkv::db::update_action::erase :
            pkv::db::update_action::write;
        allocations = 0; measuring = true;
        const auto status = db.apply(roots);
        measuring = false;
        require(status == pkv::db::batch_status::ok, "native commit");
        require(allocations == 0, "equal-length root commit allocated");
        std::string out;
        require(db.get("key", out) == !erase &&
            (erase || out == std::string(128, 'b')), "native commit bytes");
        uint64_t count = 99;
        require(db.key_count(count) == pkv::db::count_status::ok &&
            count == unsigned(!erase), "native commit roots");
        std::cout << (erase ? "erase" : "overwrite") << ": " <<
            allocations << '\n';
    }
    test_page_commit();
    test_large_growth_capacity();
    test_sealed_list_capacity();
    test_prepared_lifetime();
    test_retained_capacity();
    test_spill_commit();
    test_spill_failure();
    test_spill_lifetime();
    test_spill_shrink();
    test_spill_bounds();
    test_root_only_commit(false);
    test_root_only_commit(true);
    test_adopt_family_rollback(false);
    test_adopt_family_rollback(true);
}
