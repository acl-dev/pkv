#include "stdafx.h"
#include "family_plan_internal.h"
#include "database_pool.h"
#include "rpc/page_batch.h"
#include <set>

namespace pkv {
db::batch_status family_plan::validate(const std::vector<family_edit>& edits) {
    if (edits.empty() || edits.size() > page_batch::max_records)
        return db::batch_status::aborted;
    std::set<std::pair<unsigned, std::string>> names;
    std::set<size_t> origins;
    size_t bytes = 0, records = edits.size();
    auto charge = [&](size_t amount) {
        if (amount > page_batch::max_bytes - bytes) return false;
        bytes += amount; return true;
    };
    for (const auto& edit : edits) {
        if (edit.database >= page_batch::max_databases ||
            edit.key.size() > page_batch::max_key_bytes ||
            edit.expected_root.size() > page_transaction::max_value_bytes ||
            edit.value.size() > page_transaction::max_value_bytes ||
            (!edit.expected_found && !edit.expected_root.empty()) ||
            !names.emplace(edit.database, edit.key).second ||
            !charge(edit.key.size()) || !charge(edit.expected_root.size()) ||
            !charge(edit.value.size())) return db::batch_status::aborted;
        if (edit.action == db::update_action::erase) {
            if (edit.family != SIZE_MAX || !edit.pages.empty() ||
                !edit.value.empty()) return db::batch_status::aborted;
        } else if (edit.action != db::update_action::write)
            return db::batch_status::aborted;
        if (edit.family != SIZE_MAX && (edit.family >= edits.size() ||
            !edits[edit.family].expected_found ||
            !origins.insert(edit.family).second))
            return db::batch_status::aborted;
        uint64_t previous = 0;
        for (const auto& page : edit.pages) {
            if (!page.id || page.id <= previous || page.owner != edit.key ||
                page.value.size() > page_transaction::max_value_bytes ||
                (page.action != db::update_action::write &&
                 page.action != db::update_action::erase) ||
                (page.action == db::update_action::erase &&
                 !page.value.empty()) || records == page_batch::max_records ||
                !charge(page.owner.size()) || !charge(page.value.size()))
                return db::batch_status::aborted;
            previous = page.id; ++records;
        }
    }
    return db::batch_status::ok;
}
db::batch_status family_plan::collect(database_pool& pool,
    const std::vector<family_edit>& edits, state& out) {
    for (const auto& edit : edits) {
        if (edit.database >= pool.size()) return db::batch_status::aborted;
        out.databases.emplace(edit.database, state::database());
    }
    for (auto& entry : out.databases) {
        auto& item = entry.second;
        item.value = pool.get(entry.first);
        if (!item.value) return db::batch_status::error;
        if (!item.value->dbsupports_detached_pages() ||
            !item.value->dbsupports_owned_prepare())
            return db::batch_status::unsupported;
        if (!item.value->opened_) return db::batch_status::error;
        out.pins.push_back(item.value); out.ordered.push_back(&item);
    }
    std::sort(out.ordered.begin(), out.ordered.end(),
        [](const state::database* a, const state::database* b) {
        return std::less<db*>()(a->value.get(), b->value.get());
    });
    for (size_t i = 1; i < out.ordered.size(); ++i)
        if (out.ordered[i - 1]->value == out.ordered[i]->value)
            return db::batch_status::unsupported;
    for (const auto& edit : edits) {
        if (edit.family == SIZE_MAX) continue;
        auto& source = out.databases.at(edits[edit.family].database).value;
        auto& target = out.databases.at(edit.database).value;
        if (!source->dbsupports_family_transfer(*target))
            return db::batch_status::unsupported;
    }
    std::vector<acl::fiber_mutex*> mutexes;
    for (const auto& edit : edits)
        mutexes.push_back(&out.databases.at(edit.database).value->
            key_mutex(edit.key));
    std::sort(mutexes.begin(), mutexes.end(), std::less<acl::fiber_mutex*>());
    mutexes.erase(std::unique(mutexes.begin(), mutexes.end()), mutexes.end());
    out.guards.reserve(mutexes.size());
    for (auto* mutex : mutexes) {
        if (!mutex->lock()) return db::batch_status::error;
        out.guards.emplace_back(*mutex, std::adopt_lock);
    }
    return db::batch_status::ok;
}
db::batch_status family_plan::apply(database_pool& pool,
    const std::vector<family_edit>& edits, const observer& observe) {
    execution_guard admission(true);
    try {
        auto status = validate(edits);
        if (status != db::batch_status::ok) return status;
        state prepared;
        status = collect(pool, edits, prepared);
        if (status != db::batch_status::ok) return status;
        status = prepare(edits, prepared);
        if (status != db::batch_status::ok) return status;
        if (observe) observe(step::prepared);
        struct publication_guard {
            bool complete = false;
            ~publication_guard() { if (!complete) std::abort(); }
        } guard;
        for (auto& family : prepared.originals) family->publish();
        for (auto& family : prepared.fresh) family->publish();
        for (auto& transfer : prepared.transfers) transfer->publish();
        for (auto* item : prepared.ordered) item->frame->publish();
        if (observe) observe(step::published);
        for (auto* item : prepared.ordered)
            item->value->publish_page_effects(item->roots, {}, item->ready);
        if (observe) observe(step::completed);
        guard.complete = true;
        return db::batch_status::ok;
    } catch (...) { return db::batch_status::error; }
}
} // namespace pkv
