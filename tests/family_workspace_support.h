#pragma once
#include "family_plan_support.h"
#include "db/family_workspace.h"
#include <future>
#include <thread>

struct workspace_fixture {
    plan_trace trace;
    family_events events;
    std::vector<std::unique_ptr<db_watchers>> watchers;
    std::vector<shared_db> databases;
    std::unique_ptr<database_pool> pool;
    std::vector<memory_image> before;
    explicit workspace_fixture(bool tbb) {
        const unsigned ids[] = {0, 7, 15, 3};
        for (unsigned i = 0; i < 4; ++i) {
            watchers.emplace_back(new db_watchers(ids[i]));
            watchers.back()->add_watcher(&events);
            databases.push_back(planned_database(tbb, trace, 4 + i));
            require(databases.back()->open("", watchers.back().get()),
                "ownership workspace DB open");
            if (i < 3) seed(databases.back(), family_key,
                i == 0 ? 3 : i == 1 ? 5 : 2, family_deadline);
            before.push_back(image(databases.back(), family_key));
        }
        pool.reset(new database_pool(databases[0], 16, [this](size_t id) {
            return id == 7 ? databases[1] : id == 15 ? databases[2] :
                id == 3 ? databases[3] : shared_db();
        }));
        events.calls = 0;
        for (auto database : databases) control(database).reset();
    }
    std::vector<family_move> cycle() const {
        std::vector<family_move> moves;
        const unsigned from[] = {0, 7, 15, 3}, to[] = {3, 0, 7, 15};
        for (unsigned i = 0; i < 4; ++i) {
            family_move move;
            move.source = from[i]; move.target = to[i]; move.key = family_key;
            move.source_root = before[i < 3 ? i : 0].root;
            moves.push_back(std::move(move));
        }
        return moves;
    }
    void unchanged() {
        require(!trace.published && !events.calls,
            "workspace staged native publication");
        for (unsigned i = 0; i < 4; ++i)
            require(image(databases[i], family_key) == before[i],
                "workspace changed native prefix");
    }
    void complete() {
        for (unsigned i = 0; i < 3; ++i)
            require(image(databases[i], family_key) == before[(i + 1) % 3],
                "workspace family cycle lost identity");
        require(!image(databases[3], family_key).found &&
            !events.calls && trace.published == 4 && !trace.native_pages,
            "workspace cycle temporary/observer/native deltas");
    }
};
static family_move current_move(family_workspace& view, unsigned from,
    unsigned to, const std::string& key = family_key) {
    family_move out; out.source = from; out.target = to; out.key = key;
    require(view.lookup(from, key, out.source_root) == db::lookup_status::found,
        "staged MOVE source");
    const auto status = view.lookup(to, key, out.target_root);
    require(status != db::lookup_status::error, "staged MOVE target");
    out.target_found = status == db::lookup_status::found;
    return out;
}
static std::shared_ptr<const page_batch> workspace_delta(unsigned database,
    db::update_action action, const std::string& value,
    std::vector<page_change> pages = {}, bool reset = false,
    bool maintenance = false, const std::string& key = family_key) {
    std::vector<db::batch_entry> roots;
    roots.emplace_back(key, true, value); roots.back().action = action;
    roots.back().clear_pages = reset;
    roots.back().maintenance_only = maintenance;
    auto batch = page_batch::capture(database, roots, pages);
    require(batch != nullptr, "workspace delta capture"); return batch;
}
static void expect_page(family_workspace& view, unsigned database,
    uint64_t id, const std::string* expected) {
    std::string bytes = "unchanged";
    const auto status = view.lookup_page(database, family_key, id, bytes);
    require(status == (expected ? db::lookup_status::found :
        db::lookup_status::missing) && bytes == (expected ? *expected : ""),
        "staged page lookup did not follow family");
}
