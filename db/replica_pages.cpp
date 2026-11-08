#include "stdafx.h"
#include "db.h"
#include "page_transaction.h"
#include "db_watcher.h"
#include "common/execution_gate.h"
#include <map>

namespace pkv {
namespace {
using action = db::update_action;
bool valid_roots(const std::vector<db::batch_entry>& roots) {
    const db::batch_entry* previous = nullptr;
    for (const auto& root : roots) {
        if ((previous && previous->key >= root.key) ||
            (root.action != action::keep && root.action != action::write &&
                root.action != action::erase) ||
            (root.clear_pages && root.action != action::write) ||
            (root.maintenance_only && !root.clear_pages) ||
            root.force_watch_invalidation ||
            (root.action == action::write &&
                root.value.size() > page_transaction::max_value_bytes))
            return false;
        previous = &root;
    }
    return !roots.empty();
}
bool valid_pages(const page_update& update) {
    const page_change* previous = nullptr;
    size_t owner = 0;
    for (const auto& page : update.pages) {
        if ((previous && (page.owner < previous->owner ||
            (page.owner == previous->owner && page.id <= previous->id))) ||
            (page.action != action::write && page.action != action::erase) ||
            (page.action == action::erase && !page.value.empty()) ||
            page.id == 0 ||
            page.value.size() > page_transaction::max_value_bytes) return false;
        while (owner < update.roots.size() &&
            update.roots[owner].key < page.owner) ++owner;
        if (owner == update.roots.size() ||
            update.roots[owner].key != page.owner ||
            (page.action == action::write &&
                update.roots[owner].action == action::erase)) return false;
        previous = &page;
    }
    return true;
}
struct replay_state {
    bool present;
    bool logical = false;
    explicit replay_state(bool found) : present(found) {}
};
using states = std::map<std::string, replay_state>;

bool replay_roots(page_transaction& tx, const page_update& update,
    states& state) {
    for (const auto& source : update.roots) {
        auto& target = *tx.roots().find(source.key);
        auto& current = state.find(source.key)->second;
        if (source.action == action::keep) continue;
        if (source.clear_pages || source.action == action::erase ||
            target.action == action::erase) {
            if (!tx.reset_pages(source.key)) return false;
        }
        if (source.action == action::erase) {
            target.clear_pages = false;
            current.present = false;
            current.logical = true;
        } else {
            const bool created = !current.present;
            target.value = source.value;
            current.present = true;
            if (created || !source.maintenance_only) {
                current.logical = true;
                target.force_watch_invalidation = true;
            }
        }
        target.action = source.action;
        target.maintenance_only = !current.logical;
    }
    return true;
}
bool replay_pages(page_transaction& tx, const page_update& update,
    states& state) {
    for (const auto& page : update.pages) {
        auto& current = state.find(page.owner)->second;
        const auto& declaration = *std::lower_bound(update.roots.begin(),
            update.roots.end(), page.owner,
            [](const db::batch_entry& root, const std::string& key) {
                return root.key < key;
            });
        auto& target = *tx.roots().find(page.owner);
        if (page.action == action::write) {
            // Validate every prefix, even if a later root write would make
            // the final flattened delta appear valid on a missing base.
            if (!current.present || !tx.put(page.owner, page.id, page.value))
                return false;
        } else if (!tx.erase(page.owner, page.id)) return false;
        if (!declaration.maintenance_only) {
            current.logical = true;
            if (declaration.action != action::erase)
                target.force_watch_invalidation = true;
        }
        target.maintenance_only = !current.logical;
    }
    return true;
}
bool stage_update(page_transaction& tx, const page_update& update) {
    for (const auto& root : update.roots) {
        auto& target = *tx.roots().find(root.key);
        target.action = root.action;
        target.clear_pages = root.clear_pages;
        target.maintenance_only = root.maintenance_only;
        if (root.action == action::write) target.value = root.value;
    }
    for (const auto& page : update.pages) {
        const bool ok = page.action == action::write ?
            tx.put(page.owner, page.id, page.value) :
            tx.erase(page.owner, page.id);
        if (!ok) return false;
    }
    return true;
}

} // namespace

db::batch_status db::replica_apply_pages(
    const std::vector<batch_entry>& roots,
    const std::vector<page_change>& pages) {
    try {
        const page_update update(roots, pages);
        if (!valid_roots(roots) || !valid_pages(update))
            return batch_status::aborted;
        auto stage = [&](page_transaction& tx) {
            return stage_update(tx, update);
        };
        if (roots.size() == 1) {
            execution_guard admission;
            if (!dbsupports_pages()) return batch_status::unsupported;
            // The batch owns the key for the lifetime of the apply.
            return transact_page_keys(&roots.front().key, 1, std::ref(stage),
                false, &update, 1);
        }
        std::vector<std::string> keys;
        keys.reserve(roots.size());
        for (const auto& root : roots) keys.push_back(root.key);
        return transact_pages_impl(std::move(keys), std::ref(stage), false,
            &update, 1);
    } catch (const std::bad_alloc&) { return batch_status::error; }
}

db::batch_status db::replica_apply_pages(
    const std::vector<page_update>& updates) {
    if (updates.empty()) return batch_status::aborted;
    if (updates.size() == 1) return replica_apply_pages(updates.front().roots,
        updates.front().pages);
    try {
        std::vector<std::string> keys;
        for (const auto& update : updates) {
            if (!valid_roots(update.roots) || !valid_pages(update))
                return batch_status::aborted;
            for (const auto& root : update.roots) keys.push_back(root.key);
        }
        const auto declarations = keys.size();
        return transact_pages_impl(std::move(keys), [&](page_transaction& tx) {
            // With disjoint owners there is no inter-batch history to merge.
            // Reuse the single-batch path and its missing-owner validation.
            if (tx.roots().size() == declarations) {
                for (const auto& update : updates)
                    if (!stage_update(tx, update)) return false;
                return true;
            }
            states state;
            for (const auto& root : tx.roots())
                state.emplace(root.key, replay_state(root.found));
            for (const auto& update : updates)
                if (!replay_roots(tx, update, state) ||
                    !replay_pages(tx, update, state)) return false;
            return true;
        }, false, updates.data(), updates.size());
    } catch (const std::bad_alloc&) { return batch_status::error; }
}

db::batch_status db::commit_page_sequence(
    const std::vector<page_update>& updates) {
    execution_guard admission(true);
    if (updates.empty()) return batch_status::aborted;
    try {
        for (const auto& update : updates) {
            if (!valid_roots(update.roots) || !valid_pages(update))
                return batch_status::aborted;
        }
        if (!can_observe_page_sequence(updates))
            return batch_status::unsupported;
        const auto status = replica_apply_pages(updates);
        if (status != batch_status::ok) return status;
        observe_page_sequence(updates);
        return batch_status::ok;
    } catch (const std::bad_alloc&) { return batch_status::error; }
}

} // namespace pkv
