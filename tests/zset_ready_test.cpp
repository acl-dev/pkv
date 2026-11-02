#include "stdafx.h"
#include "common/key_wait.h"
#include "common/key_wait_batch.h"
#include "dao/paged_zset.h"
#include "dao/paged_list.h"
#include "dao/paged_string.h"
#include "db/page_workspace.h"
#include "db/db_watcher.h"
#include "db/mdb/mdb.h"
#include <cstdlib>
#include <iostream>
#include <new>
static thread_local bool reject_new = false;
static thread_local long budget = -1;
void* operator new(std::size_t size) {
    if (reject_new || !budget) throw std::bad_alloc();
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
static void mixed_grants() {
    using kind = key_wait_kind;
    const std::string a("a"), b("b");
    for (bool reverse : {false, true}) {
        auto queue = std::make_shared<key_wait_registry>(true);
        auto lists = queue->subscribe({a}, kind::list);
        auto sorted = queue->subscribe({a, b}, kind::zset);
        std::vector<key_ready_change> changes{{&a, true, -1, kind::list},
            {&b, true, -1, kind::zset}};
        if (reverse) std::reverse(changes.begin(), changes.end());
        key_wait_event event;
        auto& first = reverse ? sorted : lists;
        auto& second = reverse ? lists : sorted;
        reject_new = true;
        queue->publish(changes);
        const bool selected = first->wait(0, event) && event.ready;
        const bool blocked = !second->wait(0, event);
        first->interrupt();
        const bool promoted = second->wait(0, event) && event.ready;
        second->interrupt();
        reject_new = false;
        require(selected && blocked && promoted && !queue->subscribers(),
            "cross-type producer grants lost global order");
    }
}
static void mixed_many_keys() {
    using kind = key_wait_kind;
    auto queue = std::make_shared<key_wait_registry>(true);
    std::vector<std::string> keys;
    std::vector<key_ready_change> changes;
    for (unsigned i = 0; i < 4096; ++i) keys.push_back(std::to_string(i));
    auto lists = queue->subscribe(keys, kind::list);
    auto sorted = queue->subscribe(keys, kind::zset);
    for (const auto& key : keys)
        changes.push_back({&key, true, -1, kind::zset});
    key_wait_event event;
    reject_new = true;
    queue->publish(changes);
    const bool selected = sorted->wait(0, event) && event.ready;
    const bool blocked = !lists->wait(0, event);
    sorted->interrupt();
    const bool probe = lists->wait(0, event) && !event.ready;
    lists->interrupt();
    reject_new = false;
    require(selected && blocked && probe && !queue->subscribers(),
        "mixed-type dispatch or cancellation allocated C++ data");
    std::cout << "4096-key mixed-type grants allocated no C++ data\n";
}
static void batch_actor() {
    auto first = std::make_shared<key_wait_registry>(true);
    auto second = std::make_shared<key_wait_registry>(true);
    auto owner = first->subscribe({"a"}, key_wait_kind::list);
    auto independent = second->subscribe({"b"});
    const std::string a("a"), b("b");
    bool queued = false, finished = false;
    go[&] {
        execution_guard admission(true);
        {
            key_wait_batch deferred;
            const key_ready_change change(&a, true, -1, key_wait_kind::list);
            first->prepare(&change, 1); first->publish(&change, 1);
            key_wait_event event;
            require(!owner->wait(0, event), "owner publication escaped batch");
            queued = true;
            while (!finished) acl::fiber::yield();
        }
        key_wait_event event;
        require(owner->wait(0, event), "actor batch lost final publication");
    };
    go[&] {
        while (!queued) acl::fiber::yield();
        const key_ready_change change(&b, true, -1);
        second->publish(&change, 1);
        key_wait_event event;
        require(independent->wait(0, event), "batch leaked to another actor");
        finished = true;
    };
    acl::fiber::schedule();
    require(finished, "batch actor probe did not finish");
}
class tracked_store : public mdb {
public:
    tracked_store() : mdb(8) {}
    unsigned calls = 0, committed = 0;
protected:
    batch_status dbapply_pages(const std::vector<batch_entry>& roots,
        const std::vector<page_change>& pages) override {
        ++calls;
        for (const auto& root : roots)
            require(root.value.size() <= 16384, "large ZSet root");
        for (const auto& page : pages)
            require(page.value.size() <= 16384, "large ZSet page");
        const auto status = mdb::dbapply_pages(roots, pages);
        if (status == batch_status::ok) ++committed;
        return status;
    }
};
static void store_faults() {
    const std::vector<zset_member> values{{"one", 1},
        {std::string(40000, 'p'), 2}};
    const std::string key("source");
    unsigned failed = 0, succeeded = 0, preflight = 0, native = 0;
    for (long allowance = 0; allowance < 512; ++allowance) {
        auto tracked = std::make_shared<tracked_store>();
        shared_db source = tracked;
        require(source->open(""), "fault store open");
        auto token = source->wait_for_zsets({key, "target"});
        auto list_token = source->wait_for_lists({key, "target"});
        std::vector<watch_token> watches;
        require(source->watch({key}, watches) == db::batch_status::ok,
            "ready fault watch");
        dao::paged_zset data;
        dao::zadd_result added;
        bool ok;
        {
            execution_guard admission(true);
            {
                key_wait_batch deferred;
                try {
                    budget = allowance;
                    ok = data.add(source, key, values, {}, added);
                    budget = -1;
                } catch (const std::bad_alloc&) { budget = -1; ok = false; }
                key_wait_event event;
                require(!token->wait(0, event), "staged notification escaped");
                reject_new = true;
            }
            reject_new = false;
        }
        key_wait_event event;
        require(!list_token->wait(0, event),
            "ZSet allocation sweep notified List subscribers");
        if (ok) {
            ++succeeded;
            require(added.added == 2 && tracked->committed == 1 &&
                token->wait(0, event) && event.ready &&
                !watches[0]->valid(unix_time_ms()), "committed readiness");
            std::vector<zset_member> stored;
            require(data.range(source, key, 0, -1, false, stored) &&
                stored.size() == 2 && stored[0].member == values[0].member &&
                stored[1].member == values[1].member &&
                stored[0].score == 1 && stored[1].score == 2,
                "committed ZSet data changed");
        } else {
            ++failed;
            tracked->calls ? ++native : ++preflight;
            std::string root;
            require(!tracked->committed && !token->wait(0, event) &&
                watches[0]->valid(unix_time_ms()) &&
                source->lookup(key, root) == db::lookup_status::missing,
                "failed allocation published or committed data");
        }
    }
    require(failed && succeeded && preflight && native,
        "ready fault sweep missed a commit stage");
    std::cout << "ZSet ready store budgets: failed=" << failed <<
        " succeeded=" << succeeded << " preflight=" << preflight <<
        " native=" << native << std::endl;
}

static void partial_batch_failures() {
    unsigned failed = 0, succeeded = 0;
    for (long allowance = 0; allowance < 128; ++allowance) {
        auto tracked = std::make_shared<tracked_store>();
        shared_db source = tracked;
        require(source->open(""), "partial batch DB open");
        auto a = source->wait_for_zsets({"a"});
        auto b = source->wait_for_zsets({"b"});
        std::vector<watch_token> watches;
        require(source->watch({"b"}, watches) == db::batch_status::ok,
            "partial batch watch");
        dao::paged_zset data;
        dao::zadd_result result;
        bool ok;
        {
            execution_guard admission(true);
            {
                key_wait_batch deferred;
                require(data.add(source, "a", {{"first", 1}}, {}, result),
                    "initial committed command");
                try {
                    budget = allowance;
                    ok = data.add(source, "b", {{"second", 2}}, {}, result);
                    budget = -1;
                } catch (const std::bad_alloc&) { budget = -1; ok = false; }
                key_wait_event event;
                require(!a->wait(0, event), "partial notification escaped");
                reject_new = true;
            }
            reject_new = false;
        }
        key_wait_event event;
        require(a->wait(0, event) && event.ready,
            "later failure discarded earlier committed notification");
        require(b->wait(0, event) == ok && tracked->committed == (ok ? 2U : 1U)
            && watches[0]->valid(unix_time_ms()) == !ok,
            "partial batch published a failed command");
        long long count;
        require(data.size(source, "b", count) && count == (ok ? 1 : 0),
            "partial batch storage rollback changed");
        ok ? ++succeeded : ++failed;
    }
    require(failed && succeeded, "partial allocation sweep missed a branch");
    std::cout << "Deferred partial-write failure rollback passed\n";
}

struct recorder : db_watcher {
    std::vector<std::shared_ptr<const page_batch>> batches;
    std::vector<page_update> updates;
    bool supports_pages() const override { return true; }
    bool on_pages_database(unsigned, const std::vector<db::batch_entry>& roots,
        const std::vector<page_change>& pages) override {
        auto batch = page_batch::capture(0, roots, pages);
        require(bool(batch), "capture readiness observations");
        batches.push_back(batch);
        updates.emplace_back(batch->roots(), batch->pages()); return true;
    }
    bool on_set(const std::string&, const std::string&, bool) override {
        return true;
    }
    bool on_get(const std::string&, const std::string&, bool) override {
        return true;
    }
    bool on_del(const std::string&, bool) override { return true; }
};
static void add(shared_db& db, const std::string& key,
    const std::string& member) {
    dao::paged_zset data;
    dao::zadd_result result;
    require(data.add(db, key, {{member, 1}}, {}, result), "ZSet seed");
}
static void populate(shared_db& staging, unsigned mode) {
    dao::paged_string strings;
    if (mode == 0) require(strings.set(staging, "a", "wrong"), "type seed");
    else add(staging, "a", "old");
    add(staging, "b", "b");
    require(staging->del("a"), "delete a");
    if (mode != 2) add(staging, "a", "new");
}
static void database(shared_db source, const std::string& path) {
    recorder history;
    db_watchers watchers;
    watchers.add_watcher(&history);
    require(source->open(path.c_str(), &watchers), "ready DB open");
    for (unsigned mode = 0; mode < 3; ++mode) {
        source->del("a"); source->del("b"); history.updates.clear();
        history.batches.clear();
        auto token = source->wait_for_zsets({"a", "b"});
        key_wait_event event;
        {
            auto view = std::make_shared<page_workspace>(source);
            shared_db staging = view;
            populate(staging, mode);
            require(!token->wait(0, event), "staged ZSet woke subscriber");
            require(view->commit() == db::batch_status::ok, "ready commit");
        }
        const std::string expected = mode == 1 ? "a" : "b";
        require(token->wait(0, event) && token->refresh(event) &&
            token->key(event) == expected, "EXEC producer order lost");
        auto replay = db::create_mdb(8);
        require(replay->open(""), "replay DB open");
        auto receiver = replay->wait_for_zsets({"a", "b"});
        require(replay->replica_apply_pages(history.updates) ==
            db::batch_status::ok && receiver->wait(0, event) &&
            receiver->refresh(event) && receiver->key(event) == expected,
            "grouped replay producer order lost");
    }
    source->del("a"); source->del("b");
    auto token = source->wait_for_zsets({"a", "b"});
    auto lists = source->wait_for_lists({"a", "b"});
    dao::paged_string strings;
    key_wait_event event;
    require(strings.set(source, "a", "text") && !token->wait(0, event),
        "wrong type became actual readiness");
    require(source->del("a") && !token->wait(0, event),
        "deletion became actual readiness");
    add(source, "a", "typed");
    key_wait_event list_event;
    require(token->wait(0, event) && event.ready &&
        !lists->wait(0, list_event),
        "ZSet commit notified List subscribers");
    source->del("a");
    token->miss(event);
    dao::paged_list data;
    long long length;
    require(data.push(source, "a", {"list"}, false, false, length) &&
        lists->wait(0, event) && event.ready && !token->wait(0, event),
        "List commit notified ZSet subscribers");
}
int main(int argc, char** argv) {
    acl::acl_cpp_init();
    if (argc == 3) {
        const std::string backend(argv[1]);
        shared_db source;
        if (backend == "tbb") source = db::create_mdb_tbb(8);
        else if (backend == "rdb") source = db::create_rdb();
        else if (backend == "wdb") source = db::create_wdb();
        database(source, argv[2]);
    } else { mixed_grants(); mixed_many_keys(); batch_actor();
        store_faults(); partial_batch_failures();
        database(db::create_mdb(8), ""); }
    std::cout << "Typed ZSet readiness, bounded pages and ordered commits "
        "passed\n";
}
