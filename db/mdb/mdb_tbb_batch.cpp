#include "stdafx.h"
#include "mdb_tbb.h"
#if defined(HAS_TBB)
#include "db/page_transaction.h"
#include "db/prepared_family.h"
#include "db/detached_pages.h"
#include <algorithm>

namespace pkv {

struct mdb_tbb::commit_state : prepared_batch {
    struct owner_plan {
        const batch_entry* root;
        record_ptr data;
        store* shard;
        scan_index* index;
        const owner_value* canonical = nullptr;
        bool created = false, inserted = false, indexed = false, reset = false;
        page_map previous;
        owner_plan(const batch_entry& value, record_ptr owner, store* table,
            scan_index* keys) : root(&value), data(std::move(owner)),
            shard(table), index(keys) {}
    };
    mdb_tbb& database;
    const std::vector<batch_entry>& roots;
    const std::vector<page_change>& pages;
    const bool preserve;
    std::vector<owner_plan> owners;
    std::vector<std::unique_lock<acl::fiber_mutex>> guards;
    std::vector<std::pair<std::string*, std::string>> replacements;
    std::vector<std::pair<page_map*, uint64_t>> additions;
    bool done = false;
    commit_state(mdb_tbb& db, const std::vector<batch_entry>& r,
        const std::vector<page_change>& p, bool keep)
    : database(db), roots(r), pages(p), preserve(keep) {}
    ~commit_state() { if (!done) rollback(); }
    owner_plan* owner(const std::string& key);
    bool gather();
    bool prepare();
    void publish() noexcept override;
    std::unique_ptr<prepared_transfer> prepare_family(
        prepared_batch& target) override;
    std::unique_ptr<prepared_transfer> prepare_family(const std::string& key,
        prepared_pages& source) override;
    void rollback();
};

mdb_tbb::commit_state::owner_plan* mdb_tbb::commit_state::owner(
    const std::string& key) {
    auto found = std::lower_bound(owners.begin(), owners.end(), key,
        [](const owner_plan& plan, const std::string& name) {
        return plan.root->key < name;
    });
    return found == owners.end() || found->root->key != key ? nullptr : &*found;
}

bool mdb_tbb::commit_state::gather() {
    owners.reserve(roots.size());
    guards.reserve(roots.size());
    replacements.reserve(roots.size() + pages.size());
    additions.reserve(pages.size());
    using stripe = std::pair<size_t, acl::fiber_mutex*>;
    std::vector<stripe> stripes;
    stripes.reserve(roots.size());
    for (const auto& root : roots) {
        const auto position = database.locate(root.key);
        const owner_value* canonical = nullptr;
        auto data = database.find(root.key, &canonical);
        const bool create = !data && root.action == update_action::write;
        if (create) data = std::make_shared<record>();
        scan_index* part = nullptr;
        if (create || (data && root.action == update_action::erase)) {
            part = database.index(position, create);
            if (!part) return false;
            stripes.emplace_back(position.shard * scan_lock_count +
                position.bucket % scan_lock_count, &database.scan_lock(position));
        }
        owners.emplace_back(root, std::move(data),
            database.stores_[position.shard].get(), part);
        owners.back().created = create;
        owners.back().canonical = canonical;
    }
    std::sort(stripes.begin(), stripes.end(), [](const stripe& a, const stripe& b) {
        return a.first < b.first;
    });
    acl::fiber_mutex* previous = nullptr;
    for (const auto& item : stripes) {
        if (item.second == previous) continue;
        if (!item.second->lock()) return false;
        guards.emplace_back(*item.second, std::adopt_lock);
        previous = item.second;
    }
    return true;
}

bool mdb_tbb::commit_state::prepare() {
    for (auto& plan : owners) {
        if (plan.root->action != update_action::write) continue;
        if (!preserve || plan.root->clear_pages) {
            plan.data->pages.swap(plan.previous);
            plan.reset = true;
        }
        replacements.emplace_back(&plan.data->root, plan.root->value);
    }
    for (const auto& page : pages) {
        auto* plan = owner(page.owner);
        if (!plan) return false;
        if (plan->root->action == update_action::erase ||
            page.action != update_action::write) continue;
        if (!plan->data) return false;
        auto& values = plan->data->pages;
        auto* found = values.get(page.id);
        if (!found) {
            values.emplace(page.id, page.value);
            additions.emplace_back(&values, page.id);
        } else replacements.emplace_back(found, page.value);
    }
    for (auto& plan : owners) {
        if (!plan.created) continue;
        plan.canonical = database.add_owner(*plan.shard, *plan.index,
            plan.root->key, plan.data);
        if (!plan.canonical) return false;
        plan.inserted = true; plan.indexed = true;
    }
    return true;
}

std::unique_ptr<db::prepared_transfer>
mdb_tbb::commit_state::prepare_family(prepared_batch& destination) {
    auto* target = dynamic_cast<commit_state*>(&destination);
    if (!target || target == this || owners.size() != 1 ||
        target->owners.size() != 1 || !pages.empty() ||
        !target->pages.empty() ||
        !owners[0].root->found ||
        owners[0].root->action != update_action::erase ||
        target->owners[0].root->action != update_action::write ||
        !target->owners[0].root->clear_pages ||
        !preserve || !target->preserve ||
        !owners[0].data || !target->owners[0].data ||
        !target->owners[0].data->pages.empty()) return nullptr;
    return std::unique_ptr<prepared_transfer>(new prepared_family<page_map>(
        &owners[0].data->pages, &target->owners[0].data->pages));
}

std::unique_ptr<db::prepared_transfer>
mdb_tbb::commit_state::prepare_family(const std::string& key,
    prepared_pages& source) {
    auto* family = dynamic_cast<detached_pages<page_map>*>(&source);
    auto* target = owner(key);
    if (!family || !preserve || !pages.empty() || !target || !target->data ||
        target->root->action != update_action::write ||
        !target->root->clear_pages || !target->data->pages.empty())
        return nullptr;
    if (family->empty_after_publish()) return
        std::unique_ptr<prepared_transfer>(
            new prepared_family<page_map>(nullptr, nullptr));
    return std::unique_ptr<prepared_transfer>(new prepared_family<page_map>(
        &family->values(), &target->data->pages));
}

void mdb_tbb::commit_state::publish() noexcept {
    // All required allocation and lock acquisition finished during preparation.
    for (auto& value : replacements) value.first->swap(value.second);
    for (const auto& page : pages) {
        if (page.action != update_action::erase) continue;
        auto* plan = owner(page.owner);
        if (plan && plan->data) plan->data->pages.erase(page.id, true);
    }
    for (auto& plan : owners) {
        if (plan.root->action != update_action::erase || !plan.data) continue;
        plan.index->keys.erase(plan.canonical);
        plan.shard->owners.erase(plan.root->key);
    }
    // Promotion can relocate values. Do it after all staged pointers have
    // been consumed, and keep the old representation if allocation fails.
    for (auto first = additions.begin(); first != additions.end();) {
        auto last = first + 1;
        while (last != additions.end() && last->first == first->first)
            ++last;
        first->first->promote_prefix(static_cast<size_t>(last - first));
        first->first->promote_tail((last - 1)->second);
        first = last;
    }
    done = true;
    guards.clear(); // Reclaim old payloads after releasing SCAN index locks.
}

void mdb_tbb::commit_state::rollback() {
    for (const auto& item : additions) item.first->erase(item.second);
    for (auto& plan : owners) {
        if (plan.indexed) plan.index->keys.erase(plan.canonical);
        if (plan.inserted) plan.shard->owners.erase(plan.root->key);
        if (plan.reset) plan.data->pages.swap(plan.previous);
    }
}

db::batch_status mdb_tbb::dbprepare_pages(
    const std::vector<batch_entry>& roots,
    const std::vector<page_change>& pages, prepare_callback& ready) {
    return commit(roots, pages, true, &ready);
}
db::batch_status mdb_tbb::dbprepare_owned_pages(
    const std::vector<batch_entry>& roots,
    const std::vector<page_change>& pages,
    std::unique_ptr<prepared_batch>& out) {
    out.reset();
    try {
        std::unique_ptr<commit_state> state(
            new commit_state(*this, roots, pages, true));
        if (!state->gather() || !state->prepare()) return batch_status::error;
        out = std::move(state); return batch_status::ok;
    } catch (const std::bad_alloc&) { return batch_status::error; }
}

db::batch_status mdb_tbb::commit(const std::vector<batch_entry>& roots,
    const std::vector<page_change>& pages, bool preserve_pages,
    prepare_callback* ready) {
    try {
        commit_state state(*this, roots, pages, preserve_pages);
        if (!state.gather() || !state.prepare()) return batch_status::error;
        if (ready) return ready->prepared(state);
        state.publish();
        return batch_status::ok;
    } catch (const std::bad_alloc&) { return batch_status::error; }
}

} // namespace pkv
#endif
