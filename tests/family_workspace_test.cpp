#include "stdafx.h"
#include "family_workspace_support.h"
#include "common/key_wait.h"
#include <chrono>
#include <iostream>
#include "family_workspace_cases.h"
#include "family_workspace_slots.h"

static void cycles(bool tbb) {
    workspace_fixture files(tbb);
    std::vector<std::vector<watch_token>> watched(4);
    for (unsigned i = 0; i < 4; ++i)
        require(files.databases[i]->watch({family_key}, watched[i]) ==
            db::batch_status::ok, "workspace cycle WATCH");
    auto wake = files.databases[3]->wait_for_keys({family_key});
    family_workspace view(*files.pool);
    const auto moves = files.cycle();
    for (const auto& move : moves) {
        require(view.stage(move) == db::batch_status::ok,
            "workspace cycle stage");
        files.unchanged();
    }
    require(view.pending_records() == 12 && view.pending_bytes() > 0,
        "workspace normalized move accounting");
    const auto bytes = page_bytes(2);
    expect_page(view, 0, 2, &bytes); expect_page(view, 3, 2, nullptr);
    require(view.commit() == db::batch_status::ok && !view.failed() &&
        !view.pending_bytes() && view.commit() == db::batch_status::aborted,
        "workspace cycle commit/terminal state");
    files.complete();
    for (unsigned i = 0; i < 4; ++i)
        require(!watched[i][0]->valid(unix_time_ms()),
            "workspace lost transient key WATCH history");
    key_wait_event event;
    require(wake->wait(0, event), "workspace lost transient key wake");
    std::cout << "Ownership cycle " << tbb << " passed\n";
}
static void deltas(bool tbb) {
    workspace_fixture files(tbb);
    family_workspace view(*files.pool);
    auto patch = workspace_delta(0, db::update_action::keep, "",
        {page_change(family_key, 1, page_bytes(99), db::update_action::write)});
    require(view.stage(patch) == db::batch_status::ok, "before MOVE delta");
    patch.reset(); // The view, not the caller, retains borrowed page bytes.
    require(view.stage(workspace_delta(7, db::update_action::erase, "")) ==
        db::batch_status::ok, "target private DEL");
    require(view.stage(current_move(view, 0, 7)) == db::batch_status::ok,
        "MOVE partial family");
    const auto unchanged = page_bytes(2), changed = page_bytes(99);
    expect_page(view, 7, 1, &changed); expect_page(view, 7, 2, &unchanged);
    expect_page(view, 0, 2, nullptr); expect_page(view, 7, 5, nullptr);
    require(view.stage(workspace_delta(7, db::update_action::write,
        encoded_root(4, family_deadline), {page_change(family_key, 4,
            page_bytes(77), db::update_action::write)})) ==
        db::batch_status::ok,
        "after MOVE append");
    require(view.stage(workspace_delta(15, db::update_action::erase, "")) ==
        db::batch_status::ok && view.stage(current_move(view, 7, 15)) ==
        db::batch_status::ok, "second MOVE after delta");
    require(view.stage(workspace_delta(15, db::update_action::write,
        encoded_root(3, -1), {page_change(family_key, 4, "",
            db::update_action::erase)})) == db::batch_status::ok,
        "after second MOVE trim/PERSIST");
    expect_page(view, 15, 4, nullptr); expect_page(view, 15, 1, &changed);
    files.unchanged();
    require(view.commit() == db::batch_status::ok, "delta group commit");
    const auto result = image(files.databases[2], family_key);
    require(result.root == encoded_root(3, -1) && result.pages.size() == 3 &&
        result.pages.at(2) == files.before[0].pages.at(2) &&
        result.pages.at(3) == files.before[0].pages.at(3) &&
        !image(files.databases[0], family_key).found &&
        !image(files.databases[1], family_key).found && !files.events.calls,
        "delta fold copied/lost unchanged family pages");
}
static void faults(bool tbb) {
    unsigned failures = 0, successes = 0;
    for (long allowance = 0; allowance < 512; ++allowance) {
        workspace_fixture files(tbb);
        const auto moves = files.cycle();
        std::vector<watch_token> watched;
        require(files.databases[0]->watch({family_key}, watched) ==
            db::batch_status::ok, "ownership budget WATCH");
        db::batch_status status = db::batch_status::error;
        family_budget = allowance;
        try {
            family_workspace view(*files.pool);
            status = db::batch_status::ok;
            for (const auto& move : moves) {
                status = view.stage(move);
                if (status != db::batch_status::ok) break;
            }
            if (status == db::batch_status::ok) status = view.commit();
            else require(view.failed() && view.commit() ==
                db::batch_status::aborted, "stage error did not poison group");
        } catch (const std::bad_alloc&) {}
        family_budget = -1;
        if (status == db::batch_status::ok) {
            ++successes; files.complete();
            require(!watched[0]->valid(unix_time_ms()), "budget WATCH commit");
        } else {
            ++failures;
            require(watched[0]->valid(unix_time_ms()), "budget WATCH rollback");
            files.unchanged();
        }
    }
    require(failures && successes, "ownership budgets lack both outcomes");
    std::cout << "Ownership budgets " << tbb << " failed=" << failures
        << " succeeded=" << successes << '\n';
}
static void complexity(bool tbb) {
    size_t requests = 0, bytes = 0;
    for (size_t count : {size_t(1), size_t(64), size_t(4097)}) {
        memory_fixture files(tbb, true, false, count);
        const auto before = image(files.from, family_key);
        family_move move;
        move.source = 0; move.target = 1; move.key = family_key;
        move.source_root = before.root;
        for (auto database : {files.from, files.to}) control(database).reset();
        bool prepared = false, published = false;
        family_allocations = family_bytes = 0; family_measuring = true;
        {
            family_workspace view(*files.pool);
            require(view.stage(move) == db::batch_status::ok,
                "large ownership stage");
            require(view.commit([&](family_plan::step at) {
                if (at == family_plan::step::prepared) {
                    prepared = true; family_budget = 0;
                } else if (at == family_plan::step::published) {
                    published = true; family_budget = -1;
                }
            }) == db::batch_status::ok, "large ownership commit");
        }
        family_measuring = false; family_budget = -1;
        require(prepared && published && !files.events.calls,
            "ownership publication failed/echoed");
        for (auto database : {files.from, files.to}) {
            const auto& calls = control(database);
            require(calls.lookups == 2 && !calls.visits && !calls.page_reads &&
                !calls.page_writes && !calls.scans, "ownership copied pages");
        }
        if (count == 1) {
            requests = family_allocations; bytes = family_bytes;
        } else require(requests == family_allocations && bytes == family_bytes,
            "ownership staging scales with native family size");
        require(image(files.to, family_key) == before &&
            !image(files.from, family_key).found, "large ownership identity");
        std::cout << "Ownership view " << tbb << " pages=" << count
            << " allocations=" << family_allocations
            << " bytes=" << family_bytes << '\n';
    }
}
static void prefix(bool tbb) {
    for (unsigned kind = 0; kind < 5; ++kind) {
        workspace_fixture files(tbb);
        family_workspace view(*files.pool);
        auto moves = files.cycle();
        require(view.stage(moves[0]) == db::batch_status::ok,
            "valid prefix before malformed operation");
        if (kind == 0) moves[1].source_root += 'x';
        if (kind == 1) moves[1].target_found = true;
        if (kind == 2) moves[1].target = moves[1].source;
        if (kind == 3) moves[1].source = 16;
        if (kind == 4) moves[1].key.assign(65526, 'k');
        require(view.stage(moves[1]) == db::batch_status::aborted &&
            view.failed() && view.stage(moves[2]) ==
                db::batch_status::aborted &&
            view.commit() == db::batch_status::aborted,
            "invalid MOVE prefix rescued by suffix");
        files.unchanged();
    }
    workspace_fixture files(tbb);
    family_workspace view(*files.pool);
    require(view.stage(workspace_delta(3, db::update_action::keep, "",
        {page_change(family_key, 1, page_bytes(1), db::update_action::write)}))
        == db::batch_status::aborted && view.failed(),
        "page write to missing prefix owner accepted");
    require(view.stage(workspace_delta(3, db::update_action::write,
        encoded_root(1, -1), {}, true)) == db::batch_status::aborted &&
        view.commit() == db::batch_status::aborted,
        "later root creation rescued missing prefix page");
    files.unchanged();
}
static int injected_workspace(bool tbb, const std::string& phase) {
    workspace_fixture files(tbb);
    family_workspace view(*files.pool);
    for (const auto& move : files.cycle())
        require(view.stage(move) == db::batch_status::ok, "crash stage");
    const auto status = view.commit([&](family_plan::step at) {
        if ((phase == "prepared" && at == family_plan::step::prepared) ||
            (phase == "published" && at == family_plan::step::published) ||
            (phase == "completed" && at == family_plan::step::completed))
            throw std::runtime_error("ownership injected");
    });
    require(phase == "prepared" && status == db::batch_status::error &&
        view.failed(), "ownership post-publication ordinary error");
    files.unchanged(); return 0;
}
int main(int argc, char** argv) {
    const struct rlimit no_core = {0, 0}; ::setrlimit(RLIMIT_CORE, &no_core);
    acl::acl_cpp_init();
    if (argc == 3) return injected_workspace(std::string(argv[1]) == "tbb",
        argv[2]);
    require(argc == 1, "ownership workspace arguments");
    for (const bool tbb : {false
#ifdef HAS_TBB
        , true
#endif
    }) {
        cycles(tbb); deltas(tbb); faults(tbb); complexity(tbb); prefix(tbb);
        mixed_faults(tbb); native_failures(tbb); quotas(tbb);
        readiness(tbb, false); readiness(tbb, true); expired(tbb); typed(tbb);
        keys_and_discard(tbb); isolation(tbb); io_errors(tbb);
        capabilities(tbb);
    }
    mixed_engines(); many_destinations();
    std::cout << "Ownership view prefixes, deltas and atomic replay passed\n";
}
