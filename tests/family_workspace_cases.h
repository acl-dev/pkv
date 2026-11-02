#pragma once
#include "replica_retention_support.h"

static void mixed_faults(bool tbb) {
    unsigned failures = 0, successes = 0;
    for (long allowance = 0; allowance < 512; ++allowance) {
        memory_fixture files(tbb, true, true);
        const auto before = image(files.from, family_key);
        const auto target = image(files.to, family_key);
        family_move move;
        move.source = 0; move.target = 1; move.key = family_key;
        move.source_root = before.root;
        move.target_found = true; move.target_root = target.root;
        const std::vector<std::shared_ptr<const page_batch>> batches{
            workspace_delta(0, db::update_action::keep, "",
                {page_change(family_key, 1, page_bytes(99),
                    db::update_action::write)}),
            workspace_delta(1, db::update_action::write,
                encoded_root(4, family_deadline), {page_change(family_key, 4,
                    page_bytes(77), db::update_action::write)}),
            workspace_delta(0, db::update_action::write, encoded_root(1, -1),
                {page_change(family_key, 1, page_bytes(55),
                    db::update_action::write)}, true),
            workspace_delta(1, db::update_action::write, encoded_root(4, -1))};
        db::batch_status status = db::batch_status::error;
        family_budget = allowance;
        try {
            family_workspace view(*files.pool);
            status = view.stage(batches[0]);
            if (status == db::batch_status::ok) status = view.stage(move);
            for (size_t i = 1; i < batches.size() &&
                status == db::batch_status::ok; ++i)
                status = view.stage(batches[i]);
            if (status == db::batch_status::ok) status = view.commit();
            else require(view.failed() && view.commit() ==
                db::batch_status::aborted, "delta failure did not poison view");
        } catch (const std::bad_alloc&) {}
        family_budget = -1;
        if (status == db::batch_status::ok) {
            ++successes;
            const auto result = image(files.to, family_key);
            require(result.root == encoded_root(4, -1) &&
                result.pages.size() == 4 &&
                result.pages.at(2) == before.pages.at(2) &&
                result.pages.at(3) == before.pages.at(3) &&
                image(files.from, family_key).pages.size() == 1,
                "mixed ownership delta graph");
            dao::paged_string data; std::string bytes;
            require(data.getrange(files.from, family_key, 0, 15, bytes) &&
                bytes == page_bytes(55).substr(0, 16) &&
                data.getrange(files.to, family_key, 0, 15, bytes) &&
                bytes == page_bytes(99).substr(0, 16),
                "fresh/moved family alias");
        } else {
            ++failures;
            require(image(files.from, family_key) == before &&
                image(files.to, family_key) == target,
                "mixed delta allocation rollback");
        }
        require(!files.events.calls, "mixed delta observer echo");
    }
    require(failures && successes, "mixed budgets lack both outcomes");
    std::cout << "Ownership delta budgets " << tbb << " failed=" << failures
        << " succeeded=" << successes << '\n';
}
static void native_failures(bool tbb) {
    for (unsigned kind = 0; kind < 3; ++kind)
        for (unsigned ordinal = 1; ordinal <= (kind == 2 ? 3U : 4U);
            ++ordinal) {
            workspace_fixture files(tbb);
            family_workspace view(*files.pool);
            for (const auto& move : files.cycle())
                require(view.stage(move) == db::batch_status::ok,
                    "native failure prefix");
            if (kind == 0) files.trace.fail_detach = ordinal;
            if (kind == 1) files.trace.fail_prepare = ordinal;
            if (kind == 2) files.trace.fail_target = ordinal;
            require(view.commit() == db::batch_status::error &&
                view.failed() && view.commit() == db::batch_status::aborted,
                "native failure not terminal");
            files.unchanged();
        }
}
static void quotas(bool tbb) {
    workspace_fixture files(tbb);
    const auto move = files.cycle()[0];
    for (unsigned kind = 0; kind < 3; ++kind) {
        family_workspace view(*files.pool, kind == 0 ? 1 :
            page_batch::max_bytes, kind == 1 ? 2 : page_batch::max_records);
        const auto status = kind == 2 ? view.stage(
            std::shared_ptr<const page_batch>()) : view.stage(move);
        require(status != db::batch_status::ok && view.failed() &&
            view.commit() == db::batch_status::aborted,
            "ownership quota/null batch accepted");
        files.unchanged();
    }
    {
        family_workspace view(*files.pool, page_batch::max_bytes, 2);
        std::string bytes;
        require(view.lookup(0, family_key, bytes) == db::lookup_status::found &&
            view.lookup(7, family_key, bytes) == db::lookup_status::found &&
            view.lookup(15, family_key, bytes) == db::lookup_status::error &&
            bytes.empty() && view.failed() && view.commit() ==
                db::batch_status::aborted, "owner cache count unbounded");
    }
    {
        family_workspace view(*files.pool);
        require(view.stage(move) == db::batch_status::ok, "discard stage");
        view.discard();
        require(!view.pending_bytes() && view.commit() ==
            db::batch_status::aborted, "discard retained commit permission");
        files.unchanged();
    }
    memory_fixture cache(tbb, true, false);
    require(cache.to->set(family_key, std::string(16000, 't')), "cache seed");
    const auto source = image(cache.from, family_key);
    const auto target = image(cache.to, family_key);
    family_move large;
    large.source = 0; large.target = 1; large.key = family_key;
    large.source_root = source.root;
    large.target_found = true; large.target_root = target.root;
    family_workspace view(*cache.pool, 20000);
    require(view.stage(large) == db::batch_status::error && view.failed() &&
        view.commit() == db::batch_status::aborted &&
        image(cache.from, family_key) == source &&
        image(cache.to, family_key) == target, "root cache quota unbounded");
}
static std::vector<page_change> maintenance_image(shared_db database,
    const std::string& key) {
    std::vector<page_change> pages;
    require(database->transact_page_reads({key}, [&](page_transaction&) {
        return control(database).visit(key, [&](uint64_t id,
            const std::string& bytes) {
            pages.emplace_back(key, id, bytes, db::update_action::write);
            return true;
        }) == db::batch_status::ok;
    }) == db::batch_status::ok, "maintenance image capture");
    std::sort(pages.begin(), pages.end(),
        [](const page_change& a, const page_change& b) { return a.id < b.id; });
    return pages;
}
static void readiness(bool tbb, bool sorted) {
    memory_fixture files(tbb, true, false, 0);
    for (const auto& key : {std::string("a"), std::string("z")}) {
        if (sorted) {
            dao::paged_zset data; dao::zadd_result result;
            require(data.add(files.from, key, {{std::string(49152, 'p'), 1}},
                {}, result), "ready ZSet seed");
        } else {
            dao::paged_list data; long long size = 0;
            require(data.push(files.from, key, {std::string(49152, 'p')},
                false, false, size), "ready List seed");
        }
    }
    const auto a = image(files.from, "a"), z = image(files.from, "z");
    files.events.calls = 0;
    {
        family_workspace view(*files.pool);
        require(view.stage(current_move(view, 0, 1, "z")) ==
            db::batch_status::ok && view.stage(current_move(view, 0, 1, "a"))
            == db::batch_status::ok, "ready MOVE order");
        // Waiters can register after staging; rank is chosen at materialize.
        auto waiting = sorted ? files.to->wait_for_zsets({"a", "z"}) :
            files.to->wait_for_lists({"a", "z"});
        require(view.commit() == db::batch_status::ok &&
            image(files.to, "a") == a && image(files.to, "z") == z,
            "ready moved family");
        key_wait_event event;
        require(waiting->wait(0, event) && event.ready &&
            waiting->key(event) == "z", "local history ready rank");
    }
    std::vector<watch_token> watched;
    require(files.to->watch({"z"}, watched) == db::batch_status::ok,
        "maintenance WATCH");
    auto generic = files.to->wait_for_keys({"z"});
    auto pages = maintenance_image(files.to, "z");
    family_workspace view(*files.pool);
    require(view.stage(workspace_delta(1, db::update_action::write, z.root,
        std::move(pages), true, true, "z")) == db::batch_status::ok &&
        view.commit() == db::batch_status::ok &&
        watched[0]->valid(unix_time_ms()) && !files.events.calls,
        "maintenance lost logical history");
    key_wait_event event;
    require(!generic->wait(0, event), "maintenance signaled generic waiter");
}
static void expired(bool tbb) {
    memory_fixture files(tbb, true, false, 0);
    seed(files.from, family_key, 3, 1);
    const auto before = image(files.from, family_key);
    files.events.calls = 0;
    {
        replica_read_guard retain(true);
        family_workspace view(*files.pool);
        require(view.stage(current_move(view, 0, 1)) == db::batch_status::ok &&
            view.stage(workspace_delta(1, db::update_action::write,
                encoded_root(3, -1), {page_change(family_key, 1,
                    page_bytes(88), db::update_action::write)})) ==
                db::batch_status::ok && view.commit() == db::batch_status::ok,
            "expired authoritative MOVE + PERSIST");
    }
    const auto result = image(files.to, family_key);
    require(result.root == encoded_root(3, -1) &&
        result.pages.at(2) == before.pages.at(2) &&
        result.pages.at(3) == before.pages.at(3) && !files.events.calls,
        "expired family lost untouched pages");
    dao::paged_string data; std::string bytes;
    require(data.getrange(files.to, family_key, 0, 15, bytes) &&
        bytes == page_bytes(88).substr(0, 16), "PERSIST failed logical read");
}
static void typed(bool tbb) {
    for (unsigned type = 0; type < 5; ++type) {
        retention_pair files(tbb);
        require(files.source->run(retention_seed(type))[0] != '-' &&
            files.source->run(retention_delta(type))[0] != '-' &&
            files.changes.batches.size() >= 2, "typed source delta capture");
        require(files.changes.batches[0]->apply(*files.replica) ==
            db::batch_status::ok, "typed replica initial state");
        const auto before = image(files.replica, retention_key);
        db_watchers watchers{1}; watchers.add_watcher(&files.effects);
        auto target = memory_database(tbb, 13);
        require(target->open("", &watchers), "typed target open");
        database_pool pool(files.replica, 2, [&](size_t) { return target; });
        std::set<uint64_t> edited;
        {
            family_workspace view(pool);
            require(view.stage(current_move(view, 0, 1, retention_key)) ==
                db::batch_status::ok, "typed staged MOVE");
            for (size_t i = 1; i < files.changes.batches.size(); ++i) {
                const auto& input = *files.changes.batches[i];
                auto remapped = page_batch::capture(1, input.roots(),
                    input.pages());
                require(remapped && view.stage(remapped) ==
                    db::batch_status::ok, "typed post-MOVE delta");
                for (const auto& page : input.pages()) edited.insert(page.id);
            }
            require(view.commit() == db::batch_status::ok,
                "typed ownership commit");
        }
        retention_value(target, type);
        const auto result = image(target, retention_key);
        const auto primary = image(files.primary, retention_key);
        require(result.root == primary.root && result.pages.size() ==
            primary.pages.size(), "typed final metadata");
        size_t preserved = 0;
        for (const auto& page : result.pages) {
            require(page.second.bytes == primary.pages.at(page.first).bytes &&
                page.second.crc == primary.pages.at(page.first).crc,
                "typed final native bytes");
            if (!edited.count(page.first) && before.pages.count(page.first)) {
                require(page.second == before.pages.at(page.first),
                    "typed unchanged page copied"); ++preserved;
            }
        }
        require(preserved && !files.effects.calls &&
            !image(files.replica, retention_key).found,
            "typed source/observer/unchanged pages");
        std::cout << "Typed ownership " << tbb << " type=" << type
            << " preserved=" << preserved << '\n';
    }
}
static void keys_and_discard(bool tbb) {
    for (const auto& key : {std::string(), std::string("\0\xff", 2)}) {
        memory_fixture files(tbb, true, false, 0);
        require(files.from->set(key, ""), "raw empty root seed");
        const auto before = image(files.from, key);
        files.events.calls = 0;
        family_workspace view(*files.pool);
        std::string bytes = "sentinel";
        require(view.lookup(0, key, bytes) == db::lookup_status::found &&
            bytes.empty() && view.lookup(1, key, bytes) ==
                db::lookup_status::missing && bytes.empty(),
            "raw empty root confused with absence");
        require(view.stage(current_move(view, 0, 1, key)) ==
            db::batch_status::ok && view.commit() == db::batch_status::ok &&
            image(files.to, key) == before && !image(files.from, key).found &&
            !files.events.calls, "empty/binary ownership key");
    }
    memory_fixture files(tbb, true, false);
    const auto before = image(files.from, family_key);
    {
        family_workspace view(*files.pool);
        require(view.stage(current_move(view, 0, 1)) == db::batch_status::ok &&
            view.stage(current_move(view, 1, 0)) == db::batch_status::ok,
            "same-image MOVE round trip");
        std::vector<watch_token> watched;
        require(files.from->watch({family_key}, watched) ==
            db::batch_status::ok && view.commit() == db::batch_status::ok &&
            !watched[0]->valid(unix_time_ms()) &&
            image(files.from, family_key) == before,
            "round trip erased logical history");
    }
}
static void isolation(bool tbb) {
    memory_fixture files(tbb, true, false);
    const auto before = image(files.from, family_key);
    std::promise<void> attempted;
    auto entered = attempted.get_future();
    std::future<bool> writer;
    {
        family_workspace view(*files.pool);
        require(view.stage(current_move(view, 0, 1)) == db::batch_status::ok,
            "isolation stage");
        writer = std::async(std::launch::async, [&] {
            attempted.set_value(); return files.from->set("later", "value");
        });
        entered.get();
        require(writer.wait_for(std::chrono::milliseconds(20)) ==
            std::future_status::timeout && view.commit() ==
                db::batch_status::ok, "writer entered staged receiver view");
        require(writer.wait_for(std::chrono::milliseconds(20)) ==
            std::future_status::timeout, "view released admission too early");
    }
    require(writer.get() && image(files.to, family_key) == before,
        "writer did not resume after view destruction");
    bool staged = false, attempted_fiber = false, writer_done = false;
    go[&] {
        {
            family_workspace view(*files.pool);
            require(view.stage(current_move(view, 1, 0)) ==
                db::batch_status::ok, "fiber view stage");
            staged = true;
            while (!attempted_fiber) acl::fiber::yield();
            require(!writer_done && view.commit() == db::batch_status::ok,
                "sibling fiber entered frozen view");
        }
    };
    go[&] {
        while (!staged) acl::fiber::yield();
        attempted_fiber = true;
        require(files.to->set("fiber", "later"), "fiber writer");
        writer_done = true;
    };
    acl::fiber::schedule();
    require(writer_done && image(files.from, family_key) == before,
        "fiber view lost original family");
    std::cout << "Ownership thread/fiber admission " << tbb << " passed\n";
}
struct workspace_io_control {
    bool fail_page = false;
    virtual ~workspace_io_control() = default;
};
template<class Base> struct workspace_io_database : plan_probe<Base>,
    workspace_io_control {
    explicit workspace_io_database(plan_trace& value)
    : plan_probe<Base>(value, 4) {}
protected:
    db::lookup_status dblookup_page(const std::string& key, uint64_t id,
        std::string& out) override {
        if (this->fail_page) {
            out = "partial error output"; return db::lookup_status::error;
        }
        return family_probe<Base>::dblookup_page(key, id, out);
    }
};
static void io_errors(bool tbb) {
    plan_trace trace;
    shared_db database;
#ifdef HAS_TBB
    if (tbb) database = std::make_shared<workspace_io_database<mdb_tbb>>(trace);
#endif
    if (!database)
        database = std::make_shared<workspace_io_database<mdb>>(trace);
    require(database->open(""), "ownership IO DB open");
    seed(database, family_key, 3, family_deadline);
    const auto before = image(database, family_key);
    database_pool pool(database, 1, {});
    auto* errors = dynamic_cast<workspace_io_control*>(database.get());
    require(errors, "ownership IO control");
    {
        family_workspace view(pool); std::string bytes;
        require(view.lookup(0, family_key, bytes) == db::lookup_status::found,
            "ownership IO snapshot");
        errors->fail_page = true; bytes = "old";
        require(view.lookup_page(0, family_key, 1, bytes) ==
            db::lookup_status::error && bytes.empty() && view.failed() &&
            view.commit() == db::batch_status::aborted,
            "page IO error did not clear/poison view");
        errors->fail_page = false;
    }
    {
        control(database).fail_read = true;
        family_workspace view(pool); std::string bytes = "old";
        require(view.lookup(0, family_key, bytes) == db::lookup_status::error &&
            bytes.empty() && view.failed() && view.commit() ==
                db::batch_status::aborted, "root IO failure ignored");
        control(database).fail_read = false;
    }
    {
        family_workspace view(pool); std::string bytes = "old";
        require(view.lookup_page(0, family_key, 0, bytes) ==
            db::lookup_status::error && bytes.empty() && view.failed(),
            "zero page address accepted");
    }
    require(image(database, family_key) == before,
        "IO failure changed native state");
}
static void capabilities(bool tbb) {
    memory_fixture files(tbb, true, false);
    const auto before = image(files.from, family_key);
    family_move move;
    move.source = 0; move.target = 1; move.key = family_key;
    move.source_root = before.root;
    {
        database_pool alias(files.from, 2, [&](size_t) { return files.from; });
        family_workspace view(alias);
        require(view.stage(move) == db::batch_status::unsupported &&
            view.failed() && view.commit() == db::batch_status::aborted,
            "alias DBs accepted by ownership view");
    }
    {
        database_pool missing(files.from, 2, {});
        family_workspace view(missing);
        require(view.stage(move) == db::batch_status::error && view.failed(),
            "missing ownership target accepted");
    }
    {
        database_pool unopened(files.from, 2, [&](size_t) {
            return db::create_mdb(4);
        });
        family_workspace view(unopened);
        require(view.stage(move) == db::batch_status::error && view.failed(),
            "unopened ownership target accepted");
    }
    for (auto unsupported : {db::create_rdb(), db::create_wdb(),
        std::static_pointer_cast<db>(
            std::make_shared<page_workspace>(files.from))}) {
        database_pool pool(unsupported, 2, [&](size_t) { return files.to; });
        family_workspace view(pool);
        require(view.stage(move) == db::batch_status::unsupported &&
            view.failed() && view.commit() == db::batch_status::aborted,
            "disk/workspace ownership receiver accepted");
    }
    require(image(files.from, family_key) == before &&
        !image(files.to, family_key).found, "capability refusal changed data");
}
static void mixed_engines() {
#ifdef HAS_TBB
    plan_trace trace;
    auto left = planned_database(false, trace, 4);
    auto right = planned_database(true, trace, 5);
    require(left->open("") && right->open(""), "mixed ownership open");
    seed(left, family_key, 3, family_deadline);
    seed(right, family_key, 5, family_deadline);
    const auto before = image(left, family_key);
    const auto target = image(right, family_key);
    database_pool pool(left, 2, [&](size_t) { return right; });
    {
        family_workspace view(pool);
        require(view.stage(current_move(view, 0, 1)) ==
            db::batch_status::unsupported && view.failed() &&
            view.commit() == db::batch_status::aborted &&
            image(left, family_key) == before && image(right, family_key) ==
                target, "cross-engine native origin transferred");
    }
    {
        family_workspace view(pool);
        require(view.stage(workspace_delta(0, db::update_action::keep, "",
            {page_change(family_key, 1, page_bytes(99),
                db::update_action::write)})) == db::batch_status::ok &&
            view.stage(workspace_delta(1, db::update_action::keep, "",
                {page_change(family_key, 1, page_bytes(88),
                    db::update_action::write)})) == db::batch_status::ok &&
            view.commit() == db::batch_status::ok,
            "independent mixed native origins");
    }
    {
        family_workspace view(pool);
        require(view.stage(workspace_delta(0, db::update_action::write,
            encoded_root(1, -1), {page_change(family_key, 1, page_bytes(55),
                db::update_action::write)}, true)) == db::batch_status::ok &&
            view.stage(workspace_delta(1, db::update_action::erase, "")) ==
                db::batch_status::ok && view.stage(current_move(view, 0, 1)) ==
                db::batch_status::ok && view.commit() == db::batch_status::ok,
            "fresh portable family incorrectly treated as native handoff");
    }
    dao::paged_string data; std::string bytes;
    require(data.getrange(right, family_key, 0, 15, bytes) &&
        bytes == page_bytes(55).substr(0, 16) && !image(left, family_key).found,
        "mixed fresh data graph");
    std::cout << "Ownership mixed native/fresh capabilities passed\n";
#endif
}
