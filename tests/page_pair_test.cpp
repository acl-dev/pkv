#include "stdafx.h"
#include "db/page_transaction.h"
#include "db/page_workspace.h"
#include "db/db_watcher.h"
#include "db/mdb/mdb.h"
#include "coder/string_page.h"
#include "coder/record_codec.h"
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
using status = db::batch_status;
using action = db::update_action;
static void require(bool ok, const char* reason) {
    if (!ok) { std::cerr << reason << '\n'; std::exit(1); }
}
static const std::string key("pair\0key", 8);
static std::string header(size_t pages, long long expiry) {
    string_root root;
    root.pages = pages; root.length = pages * string_page_codec::page_bytes;
    root.expires_at_ms = expiry;
    std::string encoded;
    require(string_page_codec::encode_root(root, encoded) ==
        string_page_status::ok, "pair header");
    return encoded;
}
static void seed(shared_db database, size_t pages, char byte,
    long long expiry) {
    require(database->transact_pages({key}, [&](page_transaction& tx) {
        auto& root = *tx.roots().find(key);
        root.value = header(pages, expiry); root.action = action::write;
        require(tx.reset_pages(key), "pair seed reset");
        for (size_t i = 1; i <= pages; ++i)
            require(tx.put(key, i, std::string(16384, byte)), "pair seed page");
        return true;
    }) == status::ok, "pair seed");
}
static void check(shared_db database, size_t pages, char byte,
    long long expiry, bool present = true) {
    require(database->transact_pages({key}, [&](page_transaction& tx) {
        const auto& root = *tx.roots().find(key);
        require(root.found == present, "pair root existence");
        if (present) require(root.value == header(pages, expiry),
            "pair root/TTL changed");
        for (size_t i = 1; i <= 5; ++i) {
            std::string out("sentinel");
            const auto found = tx.lookup(key, i, out);
            require(found == (present && i <= pages ? db::lookup_status::found :
                db::lookup_status::missing), "pair private page existence");
            require(out == (present && i <= pages ? std::string(16384, byte) :
                "sentinel"), "pair private page bytes");
        }
        return true;
    }) == status::ok, "pair check");
    uint64_t count = 99;
    require(database->key_count(count) == db::count_status::ok &&
        count == unsigned(present), "pair root count");
    std::string selected; bool found = false;
    require(database->key_at_rank(0, selected, found) == db::scan_status::ok &&
        found == present && (!found || selected == key), "pair dense rank");
}
static bool transfer(page_transaction& from, page_transaction& to) {
    auto& source = *from.roots().find(key);
    auto& target = *to.roots().find(key);
    if (!source.found) return true;
    target.value = source.value; target.action = action::write;
    if (!to.reset_pages(key)) return false;
    for (uint64_t i = 1; i <= 3; ++i) {
        std::string bytes;
        if (from.lookup(key, i, bytes) != db::lookup_status::found ||
            !to.put(key, i, bytes)) return false;
    }
    source.action = action::erase;
    return true;
}
struct recorder : db_watcher {
    unsigned calls = 0;
    bool allowed = true;
    bool supports_pages() const override { return allowed; }
    bool on_pages_database(unsigned, const std::vector<db::batch_entry>&,
        const std::vector<page_change>&) override { ++calls; return true; }
    bool on_set(const std::string&, const std::string&, bool) override {
        return true;
    }
    bool on_get(const std::string&, const std::string&, bool) override {
        return true;
    }
    bool on_del(const std::string&, bool) override { return true; }
};
static shared_db create(bool tbb, db_watchers* watchers = nullptr) {
    auto db = tbb ? db::create_mdb_tbb(4) : db::create_mdb(4);
    require(db->open("", watchers), "pair open"); return db;
}
static void faults(bool left_tbb, bool right_tbb) {
    unsigned failed = 0, succeeded = 0;
    const auto expiry = unix_time_ms() + 3600000;
    for (bool existing : {false, true}) {
        const auto previous_failed = failed, previous_succeeded = succeeded;
        for (long allowance = 0; allowance < 160; ++allowance) {
            recorder events;
            db_watchers observers;
            observers.add_watcher(&events);
            auto left = create(left_tbb, &observers);
            auto right = create(right_tbb, &observers);
            seed(left, 3, 's', expiry);
            if (existing) seed(right, 5, 't', -1);
            events.calls = 0;
            std::vector<watch_token> a, b;
            require(left->watch({key}, a) == status::ok &&
                right->watch({key}, b) == status::ok, "pair WATCH");
            auto waiting_a = left->wait_for_keys({key});
            auto waiting_b = right->wait_for_keys({key});
            budget = allowance;
            const auto result = left->transact_pages_with(key, *right, key,
                transfer);
            budget = -1;
            const bool ok = result == status::ok;
            key_wait_event event;
            require(events.calls == (ok ? 2U : 0U), "pair observer boundary");
            require(a[0]->valid(unix_time_ms()) == !ok &&
                b[0]->valid(unix_time_ms()) == !ok, "pair WATCH boundary");
            require(waiting_a->wait(0, event) == ok &&
                waiting_b->wait(0, event) == ok, "pair notification boundary");
            if (ok) {
                ++succeeded;
                check(left, 0, 's', expiry, false);
                check(right, 3, 's', expiry);
            } else {
                require(result == status::error, "pair OOM returned abort");
                ++failed;
                check(left, 3, 's', expiry);
                check(right, 5, 't', -1, existing);
            }
        }
        require(failed > previous_failed && succeeded > previous_succeeded,
            "pair OOM did not cross native preparation");
    }
    std::cout << "Pair budgets " << left_tbb << right_tbb << ": failed=" <<
        failed << " succeeded=" << succeeded << '\n';
}
class inspected : public mdb {
public:
    inspected() : mdb(4) {}
    unsigned* barriers = nullptr;
    bool reject = false, reject_ready = false, cut_allocations = false;
    bool fail_reads = false;
    std::string raw() {
        std::string out;
        if (!dbget(key, out)) return "";
        return out;
    }
protected:
    lookup_status dblookup_page(const std::string& owner, uint64_t id,
        std::string& out) override {
        if (fail_reads) return lookup_status::error;
        return mdb::dblookup_page(owner, id, out);
    }
    struct bridge : prepare_callback {
        inspected& owner;
        prepare_callback& next;
        bridge(inspected& db, prepare_callback& cb) : owner(db), next(cb) {}
        batch_status prepared(prepared_batch& batch) override {
            if (owner.barriers) ++*owner.barriers;
            if (owner.reject_ready) return batch_status::error;
            if (owner.cut_allocations && owner.barriers &&
                *owner.barriers == 2) budget = 0;
            return next.prepared(batch);
        }
    };
    batch_status dbprepare_pages(const std::vector<batch_entry>& roots,
        const std::vector<page_change>& pages,
        prepare_callback& callback) override {
        if (reject) return batch_status::error;
        bridge next(*this, callback);
        return mdb::dbprepare_pages(roots, pages, next);
    }
};
static void barriers() {
    auto a = std::make_shared<inspected>(), b = std::make_shared<inspected>();
    require(a->open("") && b->open(""), "pair inspected open");
    // Fail the second engine, including after it has prepared its own changes.
    auto* second = std::less<db*>()(a.get(), b.get()) ? b.get() : a.get();
    for (unsigned mode = 0; mode < 3; ++mode) {
        seed(a, 3, 's', -1); seed(b, 5, 't', -1);
        unsigned count = 0;
        a->barriers = b->barriers = &count;
        second->reject = mode == 0;
        second->reject_ready = mode == 1;
        a->cut_allocations = b->cut_allocations = mode == 2;
        const auto result = a->transact_pages_with(key, *b, key, transfer);
        budget = -1;
        require(result == (mode == 2 ? status::ok : status::error),
            "pair explicit barrier failure");
        require(count == (mode == 0 ? 1U : 2U), "pair preparation order");
        check(a, 3, 's', -1, mode != 2);
        check(b, mode == 2 ? 3 : 5, mode == 2 ? 's' : 't', -1);
        a->barriers = b->barriers = nullptr;
    }
}
struct publication_observer : recorder {
    inspected* source = nullptr;
    inspected* target = nullptr;
    std::vector<watch_token>* source_watch = nullptr;
    std::vector<watch_token>* target_watch = nullptr;
    bool armed = false;
    bool on_pages_database(unsigned, const std::vector<db::batch_entry>&,
        const std::vector<page_change>&) override {
        ++calls;
        if (armed) {
            require(source->raw().empty() && target->raw() == header(3, -1),
                "observer ran before both native publications");
            require(!(*source_watch)[0]->valid(unix_time_ms()) &&
                !(*target_watch)[0]->valid(unix_time_ms()),
                "observer ran before both WATCH effects");
        }
        return true;
    }
};
static void observation() {
    publication_observer events;
    db_watchers observers;
    observers.add_watcher(&events);
    auto a = std::make_shared<inspected>(), b = std::make_shared<inspected>();
    require(a->open("", &observers) && b->open("", &observers),
        "pair observation open");
    seed(a, 3, 's', -1); seed(b, 5, 't', -1);
    std::vector<watch_token> x, y;
    require(a->watch({key}, x) == status::ok &&
        b->watch({key}, y) == status::ok, "pair observation WATCH");
    events.source = a.get(); events.target = b.get();
    events.source_watch = &x; events.target_watch = &y;
    events.calls = 0;
    require(a->transact_pages_with(key, *b, key,
        [](page_transaction&, page_transaction&) { return true; }) ==
        status::ok && !events.calls && x[0]->valid(unix_time_ms()) &&
        y[0]->valid(unix_time_ms()), "pair no-op published effects");
    a->fail_reads = true;
    require(a->transact_pages_with(key, *b, key, transfer) == status::error &&
        !events.calls, "pair failed read published");
    a->fail_reads = false;
    check(a, 3, 's', -1); check(b, 5, 't', -1);
    events.armed = true;
    require(a->transact_pages_with(key, *b, key, transfer) == status::ok &&
        events.calls == 2, "pair external observation boundary");
}
static void rejected() {
    auto a = create(false), b = create(false);
    seed(a, 3, 's', -1); seed(b, 5, 't', -1);
    require(a->transact_pages_with(key, *b, key,
        [](page_transaction& from, page_transaction& to) {
            transfer(from, to); return false;
        }) == status::aborted, "pair rejected callback");
    require(a->transact_pages_with(key, *b, key,
        [](page_transaction& from, page_transaction& to) {
            transfer(from, to); to.put(key, 0, "invalid"); return true;
        }) == status::aborted, "pair poisoned callback");
    check(a, 3, 's', -1); check(b, 5, 't', -1);
    require(a->transact_pages_with(key, *a, key, transfer) == status::aborted,
        "pair identical object");
}
class unprepared : public mdb {
public:
    unprepared() : mdb(4) {}
protected:
    bool dbsupports_prepare() const override { return false; }
};
static void preflight() {
    auto a = create(false);
    auto b = std::make_shared<unprepared>();
    require(b->open(""), "pair unsupported open");
    bool called = false;
    require(a->transact_pages_with(key, *b, key,
        [&](page_transaction&, page_transaction&) {
            called = true; return true;
        }) == status::unsupported && !called,
        "unsupported engine invoked pair callback");
    // Real disk engines and actor-local views must also decline before lookup.
    for (auto disk : {db::create_rdb(), db::create_wdb()}) {
        called = false;
        require(a->transact_pages_with(key, *disk, key,
            [&](page_transaction&, page_transaction&) {
                called = true; return true;
            }) == status::unsupported && !called,
            "disk engine advertised in-memory preparation");
    }
    {
        auto workspace = std::make_shared<page_workspace>(a);
        called = false;
        require(a->transact_pages_with(key, *workspace, key,
            [&](page_transaction&, page_transaction&) {
                called = true; return true;
            }) == status::unsupported && !called,
            "workspace advertised cross-DB preparation");
    }
    recorder events;
    db_watchers observers;
    observers.add_watcher(&events);
    auto c = create(false, &observers);
    seed(a, 3, 's', -1); seed(c, 5, 't', -1);
    events.calls = 0; events.allowed = false;
    require(a->transact_pages_with(key, *c, key, transfer) ==
        status::unsupported && !events.calls, "pair observer preflight");
    check(a, 3, 's', -1); check(c, 5, 't', -1);
}
static void same_database() {
    auto db = create(false);
    seed(db, 3, 's', -1);
    require(db->transact_pages_with(key, *db, "other",
        [&](page_transaction& from, page_transaction& to) {
            require(&from == &to, "same DB uses separate transactions");
            auto& source = *from.roots().find(key);
            auto& target = *to.roots().find("other");
            target.value = source.value; target.action = action::write;
            for (uint64_t i = 1; i <= 3; ++i) {
                std::string bytes;
                if (from.lookup(key, i, bytes) != db::lookup_status::found ||
                    !to.put("other", i, bytes)) return false;
            }
            source.action = action::erase; return true;
        }) == status::ok, "pair same DB native transaction");
    std::string value;
    require(!db->get(key, value) && db->get("other", value) &&
        value == header(3, -1), "pair same DB data");
}
static void concurrent(bool tbb) {
    auto a = create(tbb), b = create(tbb);
    seed(a, 3, 's', -1);
    std::thread left([&] {
        for (unsigned i = 0; i < 100; ++i)
            require(a->transact_pages_with(key, *b, key, transfer) ==
                status::ok,
                "pair concurrent forward");
    });
    std::thread right([&] {
        for (unsigned i = 0; i < 100; ++i)
            require(b->transact_pages_with(key, *a, key, transfer) ==
                status::ok,
                "pair concurrent reverse");
    });
    for (unsigned i = 0; i < 200; ++i) {
        execution_guard snapshot(true);
        uint64_t x = 99, y = 99;
        require(a->key_count(x) == db::count_status::ok &&
            b->key_count(y) == db::count_status::ok && x + y == 1,
            "pair exposed a duplicate or missing root");
    }
    left.join(); right.join();
}
int main() {
    acl::acl_cpp_init();
    barriers(); rejected(); preflight(); observation(); same_database();
    for (bool a : {false, true}) for (bool b : {false, true}) {
#ifndef HAS_TBB
        if (a || b) continue;
#endif
        faults(a, b);
    }
    concurrent(false);
#ifdef HAS_TBB
    concurrent(true);
#endif
    std::cout << "Paired preparation, rollback, pages and publication passed\n";
}
