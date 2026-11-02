#pragma once

static void fresh_faults(bool tbb) {
    unsigned failures = 0, successes = 0;
    for (bool replace : {false, true})
        for (long allowance = 0; allowance < 256; ++allowance) {
            memory_fixture files(tbb, true, replace);
            const auto before = image(files.from, family_key);
            const auto target = image(files.to, family_key);
            std::vector<family_edit> edits;
            if (replace) {
                auto value = before;
                value.root = encoded_root(2, family_deadline);
                edits.push_back(planned_edit(0, family_key, before, &value,
                    SIZE_MAX));
                for (unsigned id : {1U, 2U})
                    edits[0].pages.emplace_back(family_key, id,
                        page_bytes(90 + id), db::update_action::write);
                edits.push_back(planned_edit(1, family_key, target, nullptr,
                    SIZE_MAX));
            } else {
                edits = {planned_edit(0, family_key, before, nullptr, SIZE_MAX),
                    planned_edit(1, family_key, target, &before, 0)};
            }
            family_budget = allowance;
            const auto status = family_plan::apply(*files.pool, edits);
            family_budget = -1;
            no_page_copy(files);
            if (status == db::batch_status::ok) {
                ++successes;
                if (replace) require(image(files.from, family_key).pages.size()
                    == 2 && !image(files.to, family_key).found,
                    "fresh family replacement");
                else require(!image(files.from, family_key).found &&
                    image(files.to, family_key) == before,
                    "missing target ownership");
            } else {
                ++failures;
                require(status == db::batch_status::error &&
                    image(files.from, family_key) == before &&
                    image(files.to, family_key) == target,
                    "missing/fresh target failed rollback");
            }
        }
    require(failures && successes, "fresh plan budgets lack both paths");
    std::cout << "Fresh plan budgets " << tbb << " failed=" << failures
        << " succeeded=" << successes << '\n';
}
static void readiness(bool tbb) {
    memory_fixture files(tbb, true, false, 0);
    dao::paged_list list;
    long long length = 0;
    for (const auto& key : {std::string("a"), std::string("z")})
        require(list.push(files.from, key, {std::string(49152, 'p')}, false,
            false, length), "ready list seed");
    auto a = image(files.from, "a"), z = image(files.from, "z");
    auto absent = image(files.to, "a");
    std::vector<family_edit> edits{
        planned_edit(0, "a", a, nullptr, SIZE_MAX),
        planned_edit(0, "z", z, nullptr, SIZE_MAX),
        planned_edit(1, "a", absent, &a, 0),
        planned_edit(1, "z", absent, &z, 1)};
    edits[2].ready_rank = 5; edits[3].ready_rank = 1;
    auto waiting = files.to->wait_for_lists({"a", "z"});
    files.events.calls = 0;
    require(family_plan::apply(*files.pool, edits) == db::batch_status::ok &&
        image(files.to, "a") == a && image(files.to, "z") == z,
        "typed readiness family identity");
    key_wait_event event;
    require(waiting->wait(0, event) && waiting->key(event) == "z" &&
        event.ready, "explicit readiness rank lost");
    waiting.reset();
    auto generic = files.to->wait_for_keys({"z"});
    std::vector<watch_token> watched;
    require(files.to->watch({"z"}, watched) == db::batch_status::ok,
        "maintenance plan WATCH");
    edits = {planned_edit(1, "z", z, &z, 0)};
    edits[0].maintenance_only = true;
    require(family_plan::apply(*files.pool, edits) == db::batch_status::ok &&
        watched[0]->valid(unix_time_ms()) && !generic->wait(0, event) &&
        !files.events.calls && image(files.to, "z") == z,
        "maintenance plan changed logical history");
}
static void expired(bool tbb) {
    memory_fixture files(tbb, true, false, 0);
    seed(files.from, "old", 3, 1);
    const auto before = image(files.from, "old");
    const auto absent = image(files.to, "old");
    std::vector<family_edit> edits{
        planned_edit(0, "old", before, nullptr, SIZE_MAX),
        planned_edit(1, "old", absent, &before, 0)};
    replica_read_guard retain(true);
    require(family_plan::apply(*files.pool, edits) == db::batch_status::ok &&
        image(files.to, "old") == before && !image(files.from, "old").found,
        "authoritative plan reinterpreted expiry/read guard");
}
static void isolation(bool tbb) {
    plan_fixture files(tbb);
    std::promise<void> attempted;
    auto entered = attempted.get_future();
    std::future<bool> writer;
    const auto result = family_plan::apply(*files.pool, files.edits,
        [&](family_plan::step at) {
        if (at != family_plan::step::prepared) return;
        writer = std::async(std::launch::async, [&] {
            attempted.set_value();
            return files.databases[0]->set("later", "value");
        });
        entered.get();
        require(writer.wait_for(std::chrono::milliseconds(20)) ==
            std::future_status::timeout, "writer entered prepared plan");
    });
    require(result == db::batch_status::ok && writer.get(),
        "writer did not resume after plan");
    for (unsigned i = 0; i < 3; ++i)
        require(image(files.databases[i], family_key) ==
            files.before[(i + 1) % 3], "writer saw partial plan");
}
static int injected_plan(bool tbb, const std::string& phase) {
    plan_fixture files(tbb);
    const auto status = family_plan::apply(*files.pool, files.edits,
        [&](family_plan::step at) {
        if ((phase == "prepared" && at == family_plan::step::prepared) ||
            (phase == "published" && at == family_plan::step::published) ||
            (phase == "completed" && at == family_plan::step::completed))
            throw std::runtime_error("family plan injected");
    });
    require(phase == "prepared" && status == db::batch_status::error,
        "published plan returned an ordinary error");
    files.unchanged(); return 0;
}
static void many_owners(bool tbb) {
    plan_trace trace;
    auto database = planned_database(tbb, trace, 1);
    require(database->open(""), "many owner DB open");
    database_pool pool(database, 1, {});
    const size_t count = 300;
    std::vector<memory_image> before;
    std::vector<std::string> keys;
    for (size_t i = 0; i < count; ++i) {
        keys.push_back("many:" + std::to_string(i));
        seed(database, keys.back(), 1, family_deadline);
        before.push_back(image(database, keys.back()));
    }
    std::vector<family_edit> edits;
    for (size_t i = 0; i < count; ++i)
        edits.push_back(planned_edit(0, keys[i], before[i],
            &before[(i + 1) % count], (i + 1) % count));
    require(family_plan::apply(pool, edits) == db::batch_status::ok &&
        trace.prepared == 1 && !trace.native_pages, "many owner preparation");
    for (size_t i = 0; i < count; ++i)
        require(image(database, keys[i]) == before[(i + 1) % count],
            "many owner family permutation");
    uint64_t roots = 0;
    require(database->key_count(roots) == db::count_status::ok &&
        roots == count,
        "many owner root index count");
    std::cout << "Shared-stripe family plan " << tbb << " owners=300 passed\n";
}
static void mixed_plan() {
#ifdef HAS_TBB
    plan_trace trace;
    auto left = planned_database(false, trace, 4);
    auto right = planned_database(true, trace, 5);
    require(left->open("") && right->open(""), "mixed plan open");
    seed(left, family_key, 3, family_deadline);
    seed(right, family_key, 5, family_deadline);
    const auto a = image(left, family_key), b = image(right, family_key);
    database_pool pool(left, 2, [&](size_t) { return right; });
    std::vector<family_edit> edits{
        planned_edit(0, family_key, a, &a, 0),
        planned_edit(1, family_key, b, &b, 1)};
    require(family_plan::apply(pool, edits) == db::batch_status::ok &&
        image(left, family_key) == a && image(right, family_key) == b,
        "independent mixed-engine families");
    trace = plan_trace();
    edits[0].family = 1; edits[0].value = b.root;
    edits[1].family = 0; edits[1].value = a.root;
    require(family_plan::apply(pool, edits) == db::batch_status::unsupported &&
        !trace.detached && image(left, family_key) == a &&
        image(right, family_key) == b, "cross-engine move did not decline");
    std::cout << "Mixed-engine family plan capability passed\n";
#endif
}
static void engine_errors(bool tbb) {
    plan_fixture files(tbb);
    control(files.databases[1]).fail_read = true;
    require(family_plan::apply(*files.pool, files.edits) ==
        db::batch_status::error && !files.trace.detached,
        "bounded root read error changed families");
    control(files.databases[1]).fail_read = false;
    files.unchanged();
    database_pool missing(files.databases[0], 16, {});
    require(family_plan::apply(missing, files.edits) ==
        db::batch_status::error, "missing plan DB accepted");
    database_pool unopened(files.databases[0], 16, [&](size_t) {
        return db::create_mdb(4);
    });
    require(family_plan::apply(unopened, files.edits) ==
        db::batch_status::error, "unopened plan DB accepted");
    auto edits = files.edits; edits.resize(1); edits[0].family = 0;
    for (auto disk : {db::create_rdb(), db::create_wdb()}) {
        if (!disk) continue;
        database_pool unsupported(disk, 1, {});
        require(family_plan::apply(unsupported, edits) ==
            db::batch_status::unsupported, "unjournaled disk plan accepted");
    }
    files.unchanged();
}
