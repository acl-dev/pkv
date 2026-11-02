#include "stdafx.h"
#include "family_plan_support.h"
#include "common/key_wait.h"
#include <chrono>
#include <future>
#include <iostream>
#include <thread>
#include "family_plan_cases.h"

static void cycles(bool tbb) {
    plan_fixture files(tbb);
    std::vector<std::vector<watch_token>> watches(3);
    std::vector<std::shared_ptr<key_wait_subscription>> waiting;
    for (unsigned i = 0; i < 3; ++i) {
        require(files.databases[i]->watch({family_key}, watches[i]) ==
            db::batch_status::ok, "plan WATCH");
        waiting.push_back(files.databases[i]->wait_for_keys({family_key}));
    }
    require(family_plan::apply(*files.pool, files.edits) ==
        db::batch_status::ok, "family cycle");
    files.cycle();
    for (unsigned i = 0; i < 3; ++i) {
        key_wait_event event;
        require(!watches[i][0]->valid(unix_time_ms()) &&
            waiting[i]->wait(0, event) && event.generation > 0,
            "plan logical effects");
    }
    std::cout << "Atomic family cycle " << tbb << " passed\n";
}
static void patches(bool tbb) {
    plan_fixture files(tbb);
    auto& first = files.edits[0];
    first.value = encoded_root(4, family_deadline);
    first.pages.emplace_back(family_key, 1, page_bytes(99),
        db::update_action::write);
    first.pages.emplace_back(family_key, 5, "", db::update_action::erase);
    auto& second = files.edits[1];
    second.value = encoded_root(3, family_deadline);
    second.pages.emplace_back(family_key, 3, page_bytes(77),
        db::update_action::write);
    require(family_plan::apply(*files.pool, files.edits) ==
        db::batch_status::ok, "family patches");
    files.no_payload_copy();
    const auto a = image(files.databases[0], family_key);
    const auto b = image(files.databases[1], family_key);
    require(a.pages.size() == 4 && !a.pages.count(5) && b.pages.size() == 3,
        "family patch cardinality");
    for (auto id : {2U, 3U, 4U})
        require(a.pages.at(id) == files.before[1].pages.at(id),
            "patch copied untouched pages");
    for (auto id : {1U, 2U})
        require(b.pages.at(id) == files.before[2].pages.at(id),
            "append copied untouched pages");
    for (auto pair : {std::make_pair(0U, 99U), std::make_pair(1U, 77U)}) {
        std::string bytes;
        require(files.databases[pair.first]->transact_pages({family_key},
            [&](page_transaction& tx) {
            return tx.lookup(family_key, pair.first ? 3 : 1, bytes) ==
                db::lookup_status::found && bytes == page_bytes(pair.second);
        }) == db::batch_status::ok, "patch contents");
    }
    require(image(files.databases[2], family_key) == files.before[0],
        "unpatched cycle payload");
}
static void faults(bool tbb) {
    unsigned failed = 0, succeeded = 0;
    for (long allowance = 0; allowance < 512; ++allowance) {
        plan_fixture files(tbb);
        files.edits[0].pages.emplace_back(family_key, 1, page_bytes(99),
            db::update_action::write);
        files.edits[1].pages.emplace_back(family_key, 3, page_bytes(77),
            db::update_action::write);
        files.edits[1].value = encoded_root(3, family_deadline);
        std::vector<watch_token> watched;
        require(files.databases[0]->watch({family_key}, watched) ==
            db::batch_status::ok, "fault WATCH");
        family_budget = allowance;
        const auto result = family_plan::apply(*files.pool, files.edits);
        family_budget = -1;
        if (result == db::batch_status::ok) {
            ++succeeded; files.no_payload_copy();
            require(files.trace.published == 3 &&
                !watched[0]->valid(unix_time_ms()), "fault success effects");
        } else {
            ++failed;
            require(result == db::batch_status::error &&
                watched[0]->valid(unix_time_ms()), "fault status/WATCH");
            files.unchanged();
        }
    }
    require(failed && succeeded, "plan budgets lack both paths");
    for (unsigned kind = 0; kind < 3; ++kind)
        for (unsigned at = 1; at <= 3; ++at) {
            plan_fixture files(tbb);
            if (kind == 0) files.trace.fail_detach = at;
            if (kind == 1) files.trace.fail_prepare = at;
            if (kind == 2) files.trace.fail_target = at;
            require(family_plan::apply(*files.pool, files.edits) ==
                db::batch_status::error, "native plan injection");
            files.unchanged();
        }
    std::cout << "Family plan budgets " << tbb << " failed=" << failed
        << " succeeded=" << succeeded << '\n';
}
static void complexity(bool tbb) {
    size_t requests = 0, bytes = 0;
    for (size_t count : {size_t(1), size_t(64), size_t(4097)}) {
        memory_fixture files(tbb, true, false, count);
        const auto before = image(files.from, family_key);
        const auto absent = image(files.to, family_key);
        std::vector<family_edit> edits{
            planned_edit(0, family_key, before, nullptr, SIZE_MAX),
            planned_edit(1, family_key, absent, &before, 0)};
        for (auto db : {files.from, files.to}) control(db).reset();
        bool prepared = false, published = false;
        family_allocations = family_bytes = 0; family_measuring = true;
        const auto result = family_plan::apply(*files.pool, edits,
            [&](family_plan::step at) {
            if (at == family_plan::step::prepared) {
                prepared = true; family_budget = 0;
            } else if (at == family_plan::step::published) {
                published = true; family_budget = -1;
            }
        });
        family_measuring = false; family_budget = -1;
        require(result == db::batch_status::ok && prepared && published,
            "allocation-free family plan publication");
        no_page_copy(files);
        if (count == 1) {
            requests = family_allocations; bytes = family_bytes;
        } else require(requests == family_allocations && bytes == family_bytes,
            "family plan allocations scale with native page count");
        require(image(files.to, family_key) == before &&
            !image(files.from, family_key).found, "large family identity");
        std::cout << "Family plan " << tbb << " pages=" << count
            << " allocations=" << family_allocations
            << " bytes=" << family_bytes << '\n';
    }
}
static void same_database(bool tbb) {
    plan_trace trace;
    auto database = planned_database(tbb, trace, 1);
    require(database->open(""), "same DB open");
    const std::vector<std::string> keys{"", std::string("\0\xff", 2),
        std::string(80, 'k')};
    std::vector<memory_image> before;
    for (size_t i = 0; i < keys.size(); ++i) {
        seed(database, keys[i], i + 1, family_deadline);
        before.push_back(image(database, keys[i]));
    }
    database_pool pool(database, 1, {});
    std::vector<family_edit> edits;
    for (size_t i : {size_t(2), size_t(0), size_t(1)})
        edits.push_back(planned_edit(0, keys[i], before[i],
            &before[(i + 1) % 3], i == 2 ? 1 : i == 0 ? 2 : 0));
    require(family_plan::apply(pool, edits) == db::batch_status::ok &&
        trace.prepared == 1, "same DB family permutation");
    for (size_t i = 0; i < keys.size(); ++i)
        require(image(database, keys[i]) == before[(i + 1) % 3],
            "same DB empty/binary owner mapping");
}
static void invalid(bool tbb) {
    for (unsigned kind = 0; kind < 16; ++kind) {
        plan_fixture files(tbb);
        auto& edit = files.edits[0];
        if (kind == 0) edit.expected_root += 'x';
        if (kind == 1) edit.family = 99;
        if (kind == 2) edit.family = files.edits[1].family;
        if (kind == 3) edit.database = 16;
        if (kind == 4) edit.key.assign(65526, 'k');
        if (kind == 5) edit.value.assign(16385, 'v');
        if (kind == 6) edit.action = db::update_action::keep;
        if (kind >= 7 && kind <= 9) edit.pages.emplace_back(family_key,
            kind == 7 ? 0 : 1, "x", db::update_action::write);
        if (kind == 8) edit.pages[0].owner = "wrong";
        if (kind == 9) edit.pages.push_back(edit.pages[0]);
        if (kind == 10) {
            edit.expected_found = false; edit.expected_root.clear();
        }
        if (kind == 11) edit.action = db::update_action::erase;
        if (kind == 12) edit.pages.emplace_back(family_key, 1,
            std::string(16385, 'p'), db::update_action::write);
        if (kind == 13) edit.pages.emplace_back(family_key, 1, "x",
            db::update_action::erase);
        if (kind == 14) edit.database = 7;
        if (kind == 15) edit.action = static_cast<db::update_action>(99);
        require(family_plan::apply(*files.pool, files.edits) ==
            db::batch_status::aborted, "invalid plan accepted");
        files.unchanged();
    }
    plan_fixture files(tbb);
    auto workspace = std::make_shared<page_workspace>(files.databases[0]);
    database_pool unsupported(workspace, 1, {});
    auto edits = files.edits; edits.resize(1); edits[0].family = 0;
    require(family_plan::apply(unsupported, edits) ==
        db::batch_status::unsupported, "workspace family plan accepted");
    database_pool alias(files.databases[0], 16, [&](size_t) {
        return files.databases[0];
    });
    require(family_plan::apply(alias, files.edits) ==
        db::batch_status::unsupported, "aliased plan DBs accepted");
    files.unchanged();
}
static void typed(bool tbb) {
    memory_fixture files(tbb, true, false, 0);
    typed_recovery(files.from, files.to, [&](const std::string& key) {
        const auto before = image(files.from, key);
        const auto absent = image(files.to, key);
        std::vector<family_edit> edits{
            planned_edit(0, key, before, nullptr, SIZE_MAX),
            planned_edit(1, key, absent, &before, 0)};
        require(family_plan::apply(*files.pool, edits) ==
            db::batch_status::ok && image(files.to, key) == before &&
            !image(files.from, key).found,
            "typed ownership plan");
    });
}
int main(int argc, char** argv) {
    const struct rlimit no_core = {0, 0}; ::setrlimit(RLIMIT_CORE, &no_core);
    acl::acl_cpp_init();
    if (argc == 3) return injected_plan(std::string(argv[1]) == "tbb",
        argv[2]);
    require(argc == 1, "family plan arguments");
    for (const bool tbb : {false
#ifdef HAS_TBB
        , true
#endif
    }) {
        cycles(tbb); patches(tbb); faults(tbb); complexity(tbb);
        same_database(tbb); invalid(tbb); typed(tbb);
        fresh_faults(tbb); readiness(tbb); expired(tbb); isolation(tbb);
        many_owners(tbb); engine_errors(tbb);
    }
    mixed_plan();
    std::cout << "Atomic family permutations, patches and rollback passed\n";
}
