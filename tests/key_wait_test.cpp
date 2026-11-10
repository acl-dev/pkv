#include "stdafx.h"
#include "common/key_wait.h"
#include "dao/paged_list.h"
#include "db/mdb/mdb.h"
#include "db/page_workspace.h"
#include <atomic>
#include <chrono>
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
static void require(bool ok, const char* message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}
static void fifo() {
    auto queue = std::make_shared<key_wait_registry>();
    bool rejected = false;
    try { queue->subscribe({}); }
    catch (const std::invalid_argument&) { rejected = true; }
    require(rejected, "accepted empty wait set");
    const std::string key("binary\0key", 10);
    auto first = queue->subscribe({key, key});
    auto second = queue->subscribe({key});
    auto third = queue->subscribe({key});
    require(first->first(key) && !second->first(key) &&
        !first->first("unsubscribed"), "initial probe bypassed queue head");
    key_wait_event a, b;
    require(queue->subscribers() == 3 && !first->wait(0, a),
        "unexpected initial readiness");
    budget = 0; queue->signal(key); budget = -1;
    require(first->wait(0, a) && first->key(a) == key &&
        !second->wait(0, b), "FIFO head was bypassed");
    first.reset();
    require(second->wait(0, a) && !third->wait(0, b),
        "cancelling a claim did not promote the next waiter");
    second->miss(a);
    require(!second->wait(0, a) && !third->wait(0, b),
        "miss left stale readiness");
    queue->signal(key);
    require(second->wait(0, a), "miss lost queue position");
    second.reset();
    require(third->wait(0, b), "second cancellation stranded third");
    third.reset();
    require(!queue->subscribers(), "wait membership leaked");
}
static void initial_cancel() {
    auto queue = std::make_shared<key_wait_registry>();
    auto first = queue->subscribe({"probe"});
    auto next = queue->subscribe({"probe"});
    key_wait_event event;
    require(!next->first("probe") && !next->wait(0, event),
        "second initial probe skipped head");
    first.reset();
    require(next->first("probe") && next->wait(0, event),
        "cancelled initial probe stranded existing data");
    next->miss(event);
    require(!next->wait(0, event), "initial handoff remained ready on miss");
}
static void generations() {
    auto queue = std::make_shared<key_wait_registry>();
    auto token = queue->subscribe({"a", "b", "a"});
    key_wait_event first, second;
    queue->signal("b"); queue->signal("a");
    require(token->wait(0, first) && token->key(first) == "b",
        "multi-key event did not preserve first ready key");
    queue->signal("b"); token->miss(first);
    require(token->wait(0, second) && token->key(second) == "b" &&
        first.generation != second.generation,
        "probe/ack race lost a later commit");
    bool rejected = false;
    try { token->miss(first); }
    catch (const std::logic_error&) { rejected = true; }
    require(rejected, "stale acknowledgement accepted");
    token->miss(second);
    require(token->wait(0, second) && token->key(second) == "a",
        "miss forgot another ready key");
    token->miss(second);
    const auto start = std::chrono::steady_clock::now();
    require(!token->wait(15, first), "timeout invented an event");
    const auto elapsed = std::chrono::steady_clock::now() - start;
    require(elapsed >= std::chrono::milliseconds(10) &&
        elapsed < std::chrono::seconds(3), "wait timeout not bounded");
    queue->signal("a");
    require(token->wait(0, first), "timeout lost subscription");
}
static void overlap() {
    auto queue = std::make_shared<key_wait_registry>();
    auto first = queue->subscribe({"a", "b"});
    auto second = queue->subscribe({"b", "c"});
    auto third = queue->subscribe({"a", "c"});
    queue->signal("c"); queue->signal("b"); queue->signal("a");
    key_wait_event event;
    require(first->wait(0, event) && first->key(event) == "b",
        "overlap first choice");
    require(second->wait(0, event) && second->key(event) == "c",
        "independent ready key was unnecessarily blocked");
    require(!third->wait(0, event), "overlap bypassed both heads");
    first.reset(); second.reset();
    // Removing first makes a eligible before removing second makes c eligible.
    require(third->wait(0, event) && third->key(event) == "a",
        "later eligibility replaced an already selected key");
    third->miss(event);
    require(third->wait(0, event) && third->key(event) == "c",
        "multi-key cancellation forgot another ready key");
}
static void identity() {
    auto first = std::make_shared<key_wait_registry>();
    auto second = std::make_shared<key_wait_registry>();
    auto a = first->subscribe({"same"}), b = second->subscribe({"same"});
    first->signal("same"); second->signal("same");
    key_wait_event one, two;
    require(a->wait(0, one) && b->wait(0, two) &&
        one.slot == two.slot && one.generation == two.generation,
        "identity fixture did not align turns");
    bool rejected = false;
    try { a->miss(two); }
    catch (const std::logic_error&) { rejected = true; }
    require(rejected, "foreign database acknowledged a wait turn");
    rejected = false;
    try { a->key(two); }
    catch (const std::logic_error&) { rejected = true; }
    require(rejected, "foreign database resolved a wait key");
    a->miss(one); b->miss(two);
}
static void parallel(bool fibers) {
    const unsigned workers = 8, actors = fibers ? 4 : 1;
    auto queue = std::make_shared<key_wait_registry>();
    std::vector<key_wait_token> tokens;
    for (unsigned i = 0; i < workers * actors; ++i)
        tokens.push_back(queue->subscribe({"shared", "other"}));
    std::atomic<unsigned> sequence{0};
    auto consume = [&](unsigned index) {
        auto token = std::move(tokens[index]);
        key_wait_event event;
        require(token->wait(5000, event) && token->key(event) == "shared",
            "cross-worker wait was not signalled");
        require(sequence.fetch_add(1) == index, "cross-worker FIFO broken");
        if (fibers) acl::fiber::yield();
        token.reset();
    };
    std::vector<std::thread> threads;
    for (unsigned i = 0; i < workers; ++i) threads.emplace_back([&, i] {
        if (!fibers) { consume(i); return; }
        for (unsigned j = 0; j < actors; ++j)
            go[&, i, j] { consume(i * actors + j); };
        acl::fiber::schedule();
    });
    queue->signal("shared");
    for (auto& thread : threads) thread.join();
    require(sequence == workers * actors && !queue->subscribers(),
        "cross-worker wait did not drain");
}
static void failures() {
    unsigned failed = 0, succeeded = 0;
    for (long allowance = 0; allowance < 256; ++allowance) {
        auto queue = std::make_shared<key_wait_registry>();
        auto existing = queue->subscribe({"existing"});
        std::vector<std::string> keys{"existing"};
        for (unsigned i = 0; i < 12; ++i)
            keys.push_back(std::string(80, char('a' + i)));
        try {
            budget = allowance;
            auto token = queue->subscribe(keys);
            budget = -1; token.reset(); ++succeeded;
        } catch (const std::bad_alloc&) { budget = -1; ++failed; }
        require(queue->subscribers() == 1, "failed subscribe leaked members");
        budget = 0;
        for (const auto& key : keys) queue->signal(key);
        budget = -1;
        key_wait_event event;
        require(existing->wait(0, event) &&
            existing->key(event) == "existing", "rollback broke old waiter");
        existing->miss(event); existing.reset();
        require(!queue->subscribers(), "rollback leaked old subscription");
    }
    require(failed && succeeded, "allocation sweep covered only one outcome");
    std::cout << "Key wait budgets: failed=" << failed << " succeeded="
        << succeeded << '\n';
}
static void killed() {
    auto queue = std::make_shared<key_wait_registry>();
    auto first = queue->subscribe({"kill"});
    auto second = queue->subscribe({"kill"});
    bool waiting = false, finished = false;
    auto victim = acl::go_fiber() > [&] {
        execution_guard admission;
        auto token = std::move(first);
        key_wait_event event;
        {
            execution_pause pause;
            waiting = true;
            require(!token->wait(-1, event), "killed wait reported ready");
        }
        token.reset(); finished = true;
    };
    go[&] {
        while (!waiting) acl::fiber::yield();
        victim->kill(true);
        require(finished && queue->subscribers() == 1,
            "killed waiter failed to resume/retire");
        queue->signal("kill");
        key_wait_event event;
        require(second->wait(0, event), "killed head stranded next client");
    };
    acl::fiber::schedule();
    second.reset();
    std::weak_ptr<key_wait_registry> retained(queue);
    auto token = queue->subscribe({"lifetime"});
    queue.reset();
    require(!retained.expired(), "subscription lost registry ownership");
    token.reset();
    require(retained.expired(), "subscription retained registry after exit");
}
static void database(shared_db database, const std::string& path) {
    require(database && database->open(path.c_str()), "wait DB open");
    auto token = database->wait_for_keys({"list"});
    key_wait_event event;
    dao::paged_list data;
    long long length;
    require(database->transact_pages({"list"}, [](page_transaction&) {
        return false;
    }) == db::batch_status::aborted && !token->wait(0, event),
        "aborted write published readiness");
    {
        std::shared_ptr<page_workspace> view(new page_workspace(database));
        shared_db staging = view;
        require(data.push(staging, "list", {"uncommitted"}, false, false,
            length) && !token->wait(0, event), "staged write woke waiter");
        bool rejected = false;
        try { staging->wait_for_keys({"list"}); }
        catch (const std::logic_error&) { rejected = true; }
        require(rejected, "private workspace registered a waiter");
        view->discard();
    }
    require(!token->wait(0, event), "discard published readiness");
    {
        std::shared_ptr<page_workspace> view(new page_workspace(database));
        shared_db staging = view;
        require(data.push(staging, "list", {"committed"}, false, false,
            length) && view->commit() == db::batch_status::ok,
            "wait workspace commit");
    }
    require(token->wait(0, event), "workspace commit did not publish");
    dao::list_result result;
    require(data.pop(database, "list", true, 1, result) && result.found &&
        result.values[0] == "committed", "committed readiness has no data");
    token->miss(event);
    require(token->wait(0, event), "probe/erase race lost commit");
    token->miss(event);
    require(!token->wait(0, event), "drained list remained ready");
    require(database->replica_set("list", "replica root") &&
        token->wait(0, event), "replica root did not publish");
    token->miss(event);
    require(database->replica_del("list") && token->wait(0, event),
        "replica delete did not publish");
}
static void handoff(shared_db source) {
    const unsigned count = 16;
    std::vector<key_wait_token> tokens;
    std::vector<std::string> values;
    for (unsigned i = 0; i < count; ++i) {
        tokens.push_back(source->wait_for_keys({"jobs"}));
        values.push_back(i == count - 1 ? std::string(40000, 'x') :
            std::to_string(i));
    }
    std::atomic<unsigned> waiting{0}, completed{0};
    auto consume = [&](unsigned index) {
        execution_guard admission;
        auto token = std::move(tokens[index]);
        dao::paged_list data;
        dao::list_result out;
        require(data.pop(source, "jobs", true, 1, out) && !out.found,
            "handoff source initially nonempty");
        key_wait_event event;
        {
            execution_pause pause;
            waiting.fetch_add(1);
            require(token->wait(5000, event), "handoff waiter timed out");
        }
        require(token->key(event) == "jobs" &&
            data.pop(source, "jobs", true, 1, out) && out.found &&
            out.values[0] == values[index], "FIFO handoff lost list data");
        require(completed.fetch_add(1) == index, "handoff completion order");
        token.reset();
    };
    std::vector<std::thread> workers;
    for (unsigned thread = 0; thread < 4; ++thread)
        workers.emplace_back([&, thread] {
            for (unsigned actor = 0; actor < 4; ++actor)
                go[&, thread, actor] { consume(thread * 4 + actor); };
            acl::fiber::schedule();
        });
    while (waiting.load() != count) std::this_thread::yield();
    {
        execution_guard transaction(true);
        auto view = std::make_shared<page_workspace>(source);
        shared_db staging = view;
        dao::paged_list data;
        long long length;
        require(data.push(staging, "jobs", values, false, false, length) &&
            length == count && view->commit() == db::batch_status::ok,
            "handoff transaction commit");
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        require(!completed.load(), "handoff escaped EXEC admission");
    }
    for (auto& thread : workers) thread.join();
    require(completed.load() == count, "handoff did not drain all clients");
}
class rejected_db : public mdb {
public:
    rejected_db() : mdb(8) {}
    bool reject = true;
protected:
    batch_status dbapply_pages(const std::vector<batch_entry>& roots,
        const std::vector<page_change>& pages) override {
        return reject ? batch_status::error : mdb::dbapply_pages(roots, pages);
    }
};
static void failed_commit() {
    auto source = std::make_shared<rejected_db>();
    require(source->open(""), "rejected wait DB open");
    shared_db database = source;
    auto token = database->wait_for_keys({"list"});
    dao::paged_list data;
    long long length;
    key_wait_event event;
    require(!data.push(database, "list", {"a"}, false, false, length) &&
        !token->wait(0, event), "failed native commit published readiness");
    source->reject = false;
    require(data.push(database, "list", {"a"}, false, false, length) &&
        token->wait(0, event), "commit recovery failed to publish");
}
int main(int argc, char** argv) {
    acl::acl_cpp_init();
#ifdef __APPLE__
    // Match server startup: publish ACL's kqueue hooks before worker creation.
    std::thread runtime([] {
        go[] { acl::fiber::delay(1); };
        acl::fiber::schedule();
    });
    runtime.join();
#endif
    if (argc == 3) {
        const std::string backend(argv[1]);
        shared_db source;
        if (backend == "tbb") source = db::create_mdb_tbb(8);
        else if (backend == "rdb") source = db::create_rdb();
        else if (backend == "wdb") source = db::create_wdb();
        database(source, argv[2]);
        handoff(source);
    } else {
        fifo(); initial_cancel(); generations(); overlap(); identity();
        parallel(false); parallel(true);
        failures(); killed(); failed_commit();
        auto source = db::create_mdb(8);
        database(source, ""); handoff(source);
    }
    std::cout << "Key wait FIFO, generations, cancellation and commits "
        "passed\n";
}
