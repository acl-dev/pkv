#pragma once
#include "move_memory_support.h"
#include "db/family_plan.h"
#include <stdexcept>

struct plan_trace {
    unsigned detached = 0, prepared = 0, targets = 0, published = 0;
    unsigned fail_detach = 0, fail_prepare = 0, fail_target = 0;
    size_t native_pages = 0;
};
struct plan_frame : db::prepared_batch {
    plan_trace& trace;
    std::unique_ptr<db::prepared_batch> inner;
    explicit plan_frame(plan_trace& value) : trace(value) {}
    void publish() noexcept override { ++trace.published; inner->publish(); }
    std::unique_ptr<db::prepared_transfer> prepare_family(
        const std::string& key, db::prepared_pages& source) override {
        if (++trace.targets == trace.fail_target) return nullptr;
        return inner->prepare_family(key, source);
    }
};
template<class Base> struct plan_probe : family_probe<Base> {
    plan_trace& trace;
    plan_probe(plan_trace& value, size_t shards)
    : family_probe<Base>(shards), trace(value) {}
protected:
    db::batch_status dbdetach_pages(const std::string& key,
        std::unique_ptr<db::prepared_pages>& out) override {
        out.reset();
        if (++trace.detached == trace.fail_detach)
            return db::batch_status::error;
        return Base::dbdetach_pages(key, out);
    }
    db::batch_status dbprepare_owned_pages(
        const std::vector<db::batch_entry>& roots,
        const std::vector<page_change>& pages,
        std::unique_ptr<db::prepared_batch>& out) override {
        trace.native_pages += pages.size();
        if (++trace.prepared == trace.fail_prepare)
            return db::batch_status::error;
        std::unique_ptr<plan_frame> frame(new plan_frame(trace));
        const auto status = Base::dbprepare_owned_pages(roots, pages,
            frame->inner);
        if (status != db::batch_status::ok) return status;
        out = std::move(frame); return status;
    }
};
static shared_db planned_database(bool tbb, plan_trace& trace, size_t shards) {
#ifdef HAS_TBB
    if (tbb) return std::make_shared<plan_probe<mdb_tbb>>(trace, shards);
#else
    require(!tbb, "family plan oneTBB unavailable");
#endif
    return std::make_shared<plan_probe<mdb>>(trace, shards);
}
static family_edit planned_edit(unsigned database, const std::string& key,
    const memory_image& previous, const memory_image* value, size_t origin) {
    family_edit out;
    out.database = database; out.key = key;
    out.expected_found = previous.found; out.expected_root = previous.root;
    out.action = value ? db::update_action::write : db::update_action::erase;
    out.value = value ? value->root : ""; out.family = origin;
    return out;
}
struct plan_fixture {
    plan_trace trace;
    family_events events;
    db_watchers zero{0}, seven{7}, last{15};
    std::vector<shared_db> databases;
    std::unique_ptr<database_pool> pool;
    std::vector<memory_image> before;
    std::vector<family_edit> edits;
    explicit plan_fixture(bool tbb) {
        db_watchers* watchers[] = {&zero, &seven, &last};
        for (unsigned i = 0; i < 3; ++i) {
            databases.push_back(planned_database(tbb, trace, 4 + i));
            watchers[i]->add_watcher(&events);
            require(databases[i]->open("", watchers[i]), "plan DB open");
            seed(databases[i], family_key, i == 1 ? 5 : i == 2 ? 2 : 3,
                family_deadline);
            before.push_back(image(databases[i], family_key));
        }
        pool.reset(new database_pool(databases[0], 16, [this](size_t id) {
            return id == 7 ? databases[1] : id == 15 ? databases[2] :
                shared_db();
        }));
        for (unsigned i = 0; i < 3; ++i)
            edits.push_back(planned_edit(i == 0 ? 0 : i == 1 ? 7 : 15,
                family_key, before[i], &before[(i + 1) % 3], (i + 1) % 3));
        events.calls = 0;
        for (auto database : databases) control(database).reset();
    }
    void unchanged() {
        require(!trace.published && !events.calls, "failed plan published");
        for (unsigned i = 0; i < 3; ++i)
            require(image(databases[i], family_key) == before[i],
                "failed plan changed root/page allocation");
    }
    void no_payload_copy() {
        require(!trace.native_pages && !events.calls, "plan copied/echoed");
        for (auto database : databases) {
            const auto& call = control(database);
            require(call.lookups == 1 && !call.page_reads && !call.visits &&
                !call.page_writes && !call.scans, "plan traversed family");
        }
    }
    void cycle() {
        no_payload_copy();
        for (unsigned i = 0; i < 3; ++i)
            require(image(databases[i], family_key) == before[(i + 1) % 3],
                "family cycle changed page identity");
    }
};
