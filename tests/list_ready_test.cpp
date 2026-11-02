#include "stdafx.h"
#include "common/key_wait.h"
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
static void registry() {
    auto queue = std::make_shared<key_wait_registry>(true);
    const std::string a("a"), b("b");
    auto head = queue->subscribe({a});
    auto token = queue->subscribe({a, b});
    head.reset();
    key_wait_event event;
    require(token->wait(0, event) && token->key(event) == a,
        "cancellation probe missing");
    queue->publish({{&b, true, -1}, {&a, true, -1}});
    require(token->refresh(event) && token->key(event) == b,
        "probe preempted actual producer readiness");
    queue->publish({{&b, false, -1}});
    require(token->refresh(event) && token->key(event) == a,
        "removed list remained the trigger");
    queue->publish({{&a, false, -1}});
    token->miss(event);
    queue->publish({{&a, true, execution_time_ms() - 1}, {&b, true, -1}});
    queue->publish({{&a, true, -1}});
    require(token->wait(0, event) && token->refresh(event) &&
        token->key(event) == b, "expired readiness retained old priority");
    token->interrupt();
    require(!token->refresh(event), "interrupted token refreshed");
}
static void reservations() {
    auto queue = std::make_shared<key_wait_registry>(true);
    const std::string a("a"), b("b"), c("c");
    auto first = queue->subscribe({a});
    auto overlap = queue->subscribe({a, b});
    auto independent = queue->subscribe({c});
    queue->publish({{&b, true, -1}, {&a, true, -1}, {&c, true, -1}});
    key_wait_event event;
    require(!first->wait(0, event), "later trigger bypassed overlapping grant");
    require(independent->wait(0, event), "disjoint grant was serialized");
    require(overlap->wait(0, event) && overlap->key(event) == b,
        "first producer did not reserve conflicting keys");
    queue->publish({{&a, false, -1}});
    overlap.reset();
    require(!first->wait(0, event), "removed value woke older subscriber");
    queue->publish({{&a, true, -1}});
    require(first->wait(0, event), "grant release stranded older subscriber");
}
static void many_keys() {
    auto queue = std::make_shared<key_wait_registry>(true);
    std::vector<std::string> keys;
    for (unsigned i = 0; i < 4096; ++i) keys.push_back(std::to_string(i));
    auto token = queue->subscribe(keys);
    std::vector<key_ready_change> changes;
    for (const auto& key : keys) changes.push_back({&key, true, -1});
    key_wait_event event;
    reject_new = true;
    queue->publish(changes);
    const bool ready = token->wait(0, event);
    const bool ordered = ready && token->key(event) == keys.front();
    token->interrupt();
    reject_new = false;
    require(ordered && !queue->subscribers(), "large readiness batch failed");
    std::cout << "4096-key publication and cancellation allocated no C++ data"
        << std::endl;
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
            require(root.value.size() <= 16384, "large list root");
        for (const auto& page : pages)
            require(page.value.size() <= 16384, "large list page");
        const auto status = mdb::dbapply_pages(roots, pages);
        if (status == batch_status::ok) ++committed;
        return status;
    }
};
static void store_faults() {
    const std::vector<std::string> values{"one", std::string(40000, 'p')};
    const std::string key("source");
    unsigned failed = 0, succeeded = 0, preflight = 0, native = 0;
    for (long allowance = 0; allowance < 128; ++allowance) {
        auto tracked = std::make_shared<tracked_store>();
        shared_db source = tracked;
        require(source->open(""), "fault store open");
        auto token = source->wait_for_lists({key, "target"});
        std::vector<watch_token> watches;
        require(source->watch({key}, watches) == db::batch_status::ok,
            "ready fault watch");
        dao::paged_list data;
        long long length = 0;
        bool ok;
        try {
            budget = allowance;
            ok = data.push(source, key, values, false, false, length);
            budget = -1;
        } catch (const std::bad_alloc&) { budget = -1; ok = false; }
        key_wait_event event;
        if (ok) {
            ++succeeded;
            require(length == 2 && tracked->committed == 1 &&
                token->wait(0, event) && event.ready &&
                !watches[0]->valid(unix_time_ms()), "committed readiness");
            std::vector<std::string> stored;
            require(data.range(source, key, 0, -1, stored) && stored == values,
                "committed list data changed");
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
    std::cout << "List ready store budgets: failed=" << failed <<
        " succeeded=" << succeeded << " preflight=" << preflight <<
        " native=" << native << std::endl;
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
static void populate(shared_db& staging, unsigned mode) {
    dao::paged_list lists;
    dao::paged_string strings;
    long long length;
    if (mode == 0) require(strings.set(staging, "a", "wrong"), "type seed");
    else require(lists.push(staging, "a", {"old"}, false, false, length),
        "first a list");
    require(lists.push(staging, "b", {"b"}, false, false, length),
        "b list");
    require(staging->del("a"), "delete a");
    if (mode != 2) require(lists.push(staging, "a", {"new"}, false, false,
        length), "last a list");
}
static void database(shared_db source, const std::string& path) {
    recorder history;
    db_watchers watchers;
    watchers.add_watcher(&history);
    require(source->open(path.c_str(), &watchers), "ready DB open");
    for (unsigned mode = 0; mode < 3; ++mode) {
        source->del("a"); source->del("b"); history.updates.clear();
        history.batches.clear();
        auto token = source->wait_for_lists({"a", "b"});
        key_wait_event event;
        {
            auto view = std::make_shared<page_workspace>(source);
            shared_db staging = view;
            populate(staging, mode);
            require(!token->wait(0, event), "staged list woke subscriber");
            require(view->commit() == db::batch_status::ok, "ready commit");
        }
        const std::string expected = mode == 1 ? "a" : "b";
        require(token->wait(0, event) && token->refresh(event) &&
            token->key(event) == expected, "EXEC producer order lost");
        auto replay = db::create_mdb(8);
        require(replay->open(""), "replay DB open");
        auto receiver = replay->wait_for_lists({"a", "b"});
        require(replay->replica_apply_pages(history.updates) ==
            db::batch_status::ok && receiver->wait(0, event) &&
            receiver->refresh(event) && receiver->key(event) == expected,
            "grouped replay producer order lost");
    }
    source->del("a"); source->del("b");
    auto token = source->wait_for_lists({"a", "b"});
    dao::paged_string strings;
    key_wait_event event;
    require(strings.set(source, "a", "text") && !token->wait(0, event),
        "wrong type became actual readiness");
    require(source->del("a") && !token->wait(0, event),
        "deletion became actual readiness");
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
    } else { registry(); reservations(); many_keys(); store_faults();
        database(db::create_mdb(8), ""); }
    std::cout << "Typed list readiness, expiry and ordered commits passed\n";
}
