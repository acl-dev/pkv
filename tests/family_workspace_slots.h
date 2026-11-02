#pragma once

struct workspace_slot_metrics {
    size_t calls = 0, requests = 0, bytes = 0;
};
struct workspace_slot_frame : db::prepared_batch {
    workspace_slot_metrics& metrics;
    std::unique_ptr<db::prepared_batch> inner;
    explicit workspace_slot_frame(workspace_slot_metrics& value)
    : metrics(value) {}
    void publish() noexcept override { inner->publish(); }
    std::unique_ptr<db::prepared_transfer> prepare_family(
        const std::string& key, db::prepared_pages& source) override {
        family_allocations = family_bytes = 0; family_measuring = true;
        try {
            auto result = inner->prepare_family(key, source);
            family_measuring = false;
            ++metrics.calls; metrics.requests += family_allocations;
            metrics.bytes += family_bytes; return result;
        } catch (...) { family_measuring = false; throw; }
    }
};
struct workspace_slot_database : family_probe<mdb> {
    workspace_slot_metrics& metrics;
    explicit workspace_slot_database(workspace_slot_metrics& value)
    : family_probe<mdb>(1), metrics(value) {}
protected:
    db::batch_status dbprepare_owned_pages(
        const std::vector<db::batch_entry>& roots,
        const std::vector<page_change>& pages,
        std::unique_ptr<db::prepared_batch>& out) override {
        std::unique_ptr<workspace_slot_frame> frame(
            new workspace_slot_frame(metrics));
        const auto status = mdb::dbprepare_owned_pages(roots, pages,
            frame->inner);
        if (status != db::batch_status::ok) return status;
        out = std::move(frame); return status;
    }
};
static void many_destinations() {
    size_t single_bytes = 0;
    for (size_t count : {size_t(1), size_t(64), size_t(300)}) {
        workspace_slot_metrics metrics;
        auto from = std::make_shared<workspace_slot_database>(metrics);
        auto to = std::make_shared<workspace_slot_database>(metrics);
        require(from->open("") && to->open(""), "slot DB open");
        database_pool pool(from, 2, [&](size_t) { return to; });
        std::vector<std::string> keys;
        std::vector<memory_image> before;
        for (size_t i = 0; i < count; ++i) {
            keys.push_back("owner:" + std::to_string(i));
            seed(from, keys.back(), 1, family_deadline);
            before.push_back(image(from, keys.back()));
        }
        {
            family_workspace view(pool);
            for (size_t i = 0; i < count; ++i) {
                family_move move;
                move.source = 0; move.target = 1; move.key = keys[i];
                move.source_root = before[i].root;
                require(view.stage(move) == db::batch_status::ok,
                    "many destination staging");
            }
            require(view.commit() == db::batch_status::ok &&
                metrics.calls == count, "many destination commit");
        }
        for (size_t i = 0; i < count; ++i)
            require(image(to, keys[i]) == before[i] &&
                !image(from, keys[i]).found, "many destination ownership");
        if (count == 1) single_bytes = metrics.bytes;
        require(metrics.bytes <= 4 * single_bytes * count,
            "destination rollback bookkeeping grew quadratically");
        std::cout << "Ownership slots count=" << count << " requests="
            << metrics.requests << " bytes=" << metrics.bytes << '\n';
    }
}
