#include "stdafx.h"
#include "family_plan_internal.h"
#include <numeric>

namespace pkv {
db::batch_status family_plan::prepare(const std::vector<family_edit>& edits,
    state& out) {
    // Verify every expectation and finish allocating logical readiness before
    // detaching anything or retaining native shard/index locks.
    std::vector<size_t> ordered(edits.size());
    std::iota(ordered.begin(), ordered.end(), 0);
    std::sort(ordered.begin(), ordered.end(), [&](size_t a, size_t b) {
        return edits[a].database == edits[b].database ?
            edits[a].key < edits[b].key : edits[a].database < edits[b].database;
    });
    for (auto position : ordered) {
        const auto& edit = edits[position];
        auto& item = out.databases.at(edit.database);
        std::string original;
        const auto found = item.value->dblookup_bounded_root(edit.key,
            original);
        if (found == db::lookup_status::error) return db::batch_status::error;
        if ((found == db::lookup_status::found) != edit.expected_found ||
            (edit.expected_found && original != edit.expected_root))
            return db::batch_status::aborted;
        item.roots.emplace_back(edit.key, edit.expected_found, edit.value);
        auto& root = item.roots.back();
        root.action = edit.action;
        root.clear_pages = edit.action == db::update_action::write;
        root.maintenance_only = edit.maintenance_only;
        root.force_watch_invalidation = edit.force_watch_invalidation;
        item.ranks.push_back(edit.ready_rank);
    }
    for (auto* item : out.ordered) {
        item->ready = item->value->prepare_ready(item->roots);
        if (item->ready.size() > 1) {
            std::vector<size_t> positions(item->ready.size());
            std::iota(positions.begin(), positions.end(), 0);
            std::stable_sort(positions.begin(), positions.end(),
                [&](size_t a, size_t b) {
                return (item->ready[a].ready ? item->ranks[a] : SIZE_MAX) <
                    (item->ready[b].ready ? item->ranks[b] : SIZE_MAX);
            });
            std::vector<key_ready_change> ready;
            ready.reserve(positions.size());
            for (auto position : positions)
                ready.push_back(item->ready[position]);
            item->ready.swap(ready);
        }
        if (item->value->ready_registry_)
            item->value->ready_registry_->prepare(item->ready.data(),
                item->ready.size());
    }
    out.originals.resize(edits.size()); out.families.resize(edits.size());
    out.fresh.reserve(edits.size()); out.transfers.reserve(edits.size());
    for (size_t i = 0; i < edits.size(); ++i) {
        const auto& edit = edits[i];
        auto& database = out.databases.at(edit.database).value;
        const auto status = database->dbdetach_pages(edit.key,
            out.originals[i]);
        if (status != db::batch_status::ok) return status;
        if (!out.originals[i]) return db::batch_status::error;
    }
    for (size_t i = 0; i < edits.size(); ++i) {
        const auto& edit = edits[i];
        if (edit.action == db::update_action::erase) continue;
        auto* family = edit.family == SIZE_MAX ? nullptr :
            out.originals[edit.family].get();
        if (!family) {
            auto created = out.originals[i]->fresh();
            if (!created) return db::batch_status::error;
            family = created.get(); out.fresh.push_back(std::move(created));
        }
        const auto status = family->prepare(edit.pages);
        if (status != db::batch_status::ok) return status;
        out.families[i] = family;
    }
    for (auto* item : out.ordered) {
        const auto status = item->value->dbprepare_owned_pages(item->roots,
            item->pages, item->frame);
        if (status != db::batch_status::ok) return status;
        if (!item->frame) return db::batch_status::error;
    }
    for (size_t i = 0; i < edits.size(); ++i) {
        if (!out.families[i]) continue;
        auto& frame = out.databases.at(edits[i].database).frame;
        auto transfer = frame->prepare_family(edits[i].key, *out.families[i]);
        if (!transfer) return db::batch_status::error;
        out.transfers.push_back(std::move(transfer));
    }
    return db::batch_status::ok;
}
} // namespace pkv
