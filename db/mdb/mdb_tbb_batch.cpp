#include "stdafx.h"
#include "mdb_tbb.h"
#if defined(HAS_TBB)
#include "coder/map_page.h"
#include "db/page_transaction.h"
#include "db/prepared_family.h"
#include "db/detached_pages.h"
#include "replacement_workspace.h"
#include <algorithm>

namespace pkv {

struct mdb_tbb::commit_state : prepared_batch {
    struct owner_plan {
        const batch_entry* root;
        record* data;
        store* shard;
        scan_index* index;
        const owner_value* canonical = nullptr;
        bool created = false, reset = false;
        uint8_t bucket;
        page_map previous;
        owner_plan(const batch_entry& value, record* owner, store* table,
            scan_index* keys, size_t slot) : root(&value), data(owner),
            shard(table), index(keys), bucket(static_cast<uint8_t>(slot)) {}
    };
    mdb_tbb& database;
    const std::vector<batch_entry>& roots;
    const std::vector<page_change>& pages;
    const bool preserve;
    std::vector<owner_plan> owners;
    std::vector<std::unique_lock<acl::fiber_mutex>> guards;
    std::vector<std::pair<std::string*, std::string>> replacements;
    std::vector<std::pair<page_map*, uint64_t>> additions;
    std::vector<page_change>* adopt_pages = nullptr;
    bool dense_replacement = false;
    bool done = false;
    commit_state(mdb_tbb& db, const std::vector<batch_entry>& r,
        const std::vector<page_change>& p, bool keep)
    : database(db), roots(r), pages(p), preserve(keep) {}
    ~commit_state() { if (!done) rollback(); }
    owner_plan* owner(const std::string& key);
    const batch_entry* dense_replacement_root() const;
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

const db::batch_entry* mdb_tbb::commit_state::dense_replacement_root() const {
    if (pages.size() < 16) return nullptr;
    const batch_entry* target = nullptr;
    for (const auto& root : roots) {
        if (root.action == update_action::keep) continue;
        if (target || root.action != update_action::write ||
            (preserve && !root.clear_pages)) return nullptr;
        target = &root;
    }
    if (!target) return nullptr;
    uint64_t id = 0;
    for (const auto& page : pages) {
        if (page.id != ++id || page.action != update_action::write ||
            page.owner != target->key) return nullptr;
    }
    return target;
}

bool mdb_tbb::commit_state::gather() {
    const auto* target = dense_replacement_root();
    dense_replacement = target != nullptr;
    const auto owner_count = dense_replacement ? 1 : roots.size();
    owners.reserve(owner_count);
    guards.reserve(owner_count);
    replacements.reserve(owner_count + (dense_replacement ? 0 : pages.size()));
    if (!dense_replacement) additions.reserve(pages.size());
    using stripe = std::pair<size_t, acl::fiber_mutex*>;
    std::vector<stripe> stripes;
    stripes.reserve(owner_count);
    for (const auto& root : roots) {
        // The caller already holds the logical key locks, including readers.
        if (target && &root != target) continue;
        const auto position = database.locate(root.key);
        const owner_value* canonical = nullptr;
        auto data = database.find(root.key, &canonical);
        const bool create = !data && root.action == update_action::write;
        scan_index* part = nullptr;
        if (create || (data && root.action == update_action::erase)) {
            part = database.index(position, create);
            if (!part) return false;
            stripes.emplace_back(scan_stripe(position), &database.scan_lock(position));
        }
        owners.emplace_back(root, data,
            database.shard_at(position.shard), part, position.bucket);
        owners.back().created = create;
        owners.back().canonical = canonical;
    }
    // Sort by the shared stripe, so aliases are adjacent and acquired only once.
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
    // Keep the scan stripes and logical key locks until the new records are
    // initialized or rolled back. Staged pointers borrow the node records.
    for (auto& plan : owners) {
        if (!plan.created) continue;
        plan.canonical = database.add_owner(*plan.shard, plan.bucket, *plan.index,
            plan.root->key);
        if (!plan.canonical) return false;
        plan.data = &plan.canonical->second.data;
    }
    for (auto& plan : owners) {
        if (plan.root->action != update_action::write) continue;
        if (!preserve || plan.root->clear_pages) {
            plan.data->pages.swap(plan.previous);
            plan.reset = true;
        }
        replacements.emplace_back(&plan.data->root, plan.root->value);
    }
    const std::string empty;
    if (dense_replacement) {
        // Rollback restores the previous index as a unit, so no new page
        // addresses or IDs need to be recorded in the batch journal.
        auto& plan = owners.front();
        assert(plan.reset && plan.data->pages.empty());
        plan.data->pages.build_consecutive(pages.begin(), pages.size(),
            [&](const page_change& page) -> const std::string& {
                return adopt_pages ? empty : page.value;
            });
        return true;
    }
    const page_change* short_tail = nullptr;
    for (const auto& page : pages) {
        auto* plan = owner(page.owner);
        if (!plan) return false;
        if (plan->root->action == update_action::erase ||
            page.action != update_action::write) continue;
        if (!plan->data) return false;
        auto& values = plan->data->pages;
        auto* found = values.get(page.id);
        if (!found) {
            size_t consecutive = 0;
            if (page.id == 1 && values.empty()) {
                const auto* end = pages.data() + pages.size();
                if (end - &page >= 16) {
                    auto next = &page + 1;
                    while (next != end &&
                        next->id == static_cast<uint64_t>(next - &page) + 1 &&
                        next->action == update_action::write && next->owner == page.owner)
                        ++next;
                    consecutive = static_cast<size_t>(next - &page);
                    const auto offset = page_map::sparse_tail_offset(consecutive);
                    if (offset) short_tail = &page + offset;
                }
            }
            // The plan proves this final block is short. Skip opening it at
            // the first slot; later transactions can still promote it.
            values.emplace(page.id, adopt_pages ? empty : page.value,
                consecutive, &page != short_tail);
            additions.emplace_back(&values, page.id);
        } else replacements.emplace_back(found, page.value);
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
    if (adopt_pages) {
        if (dense_replacement) {
            auto& values = owners.front().data->pages;
            for (auto& page : *adopt_pages) values.get(page.id)->swap(page.value);
        } else {
            assert(adopt_pages->size() == additions.size());
            for (size_t i = 0; i < additions.size(); ++i) {
                const auto& item = additions[i];
                item.first->get(item.second)->swap((*adopt_pages)[i].value);
            }
        }
    }
    for (auto& value : replacements) value.first->swap(value.second);
    if (!dense_replacement) for (const auto& page : pages) {
        if (page.action != update_action::erase) continue;
        auto* plan = owner(page.owner);
        if (plan && plan->data) {
            plan->data->pages.erase(page.id, true);
            if (plan->root->action != update_action::erase)
                plan->data->pages.reclaim_prefix();
        }
    }
    for (auto& plan : owners) {
        if (plan.root->action != update_action::erase || !plan.data) continue;
        plan.data->pages.swap(plan.previous);
        database.erase_scan_key(*plan.shard, plan.bucket, *plan.index, plan.canonical);
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
        if (plan.reset) plan.data->pages.swap(plan.previous);
        if (!plan.created || !plan.canonical) continue;
        database.erase_scan_key(*plan.shard, plan.bucket, *plan.index, plan.canonical);
        plan.shard->owners.erase(plan.root->key);
    }
}

db::batch_status mdb_tbb::dbprepare_pages(
    const std::vector<batch_entry>& roots,
    const std::vector<page_change>& pages, prepare_callback& ready) {
    return commit(roots, pages, true, &ready);
}

db::batch_status mdb_tbb::dbadopt_page_family(
    const std::vector<batch_entry>& roots, std::vector<page_change>& pages) {
    if (typeid(*this) != typeid(mdb_tbb) || pages.empty())
        return batch_status::unsupported;
    const auto& key = pages.front().owner;
    const auto root = std::find_if(roots.begin(), roots.end(),
        [&](const batch_entry& entry) { return entry.key == key; });
    if (root == roots.end() || !root->clear_pages || root->action != update_action::write)
        return batch_status::unsupported;
    const bool string_family = root->value.compare(0, 4, "PSR\1", 4) == 0;
    if (!string_family) {
        map_root metadata;
        if (pages.size() < 32 || map_page_codec::decode_root(root->value, metadata) !=
            map_page_status::ok || metadata.kind != map_kind::set)
            return batch_status::unsupported;
    }
    if (roots.size() == 1 && detail::can_adopt_small_string_family(*root, pages)) {
        auto* owner = find(root->key);
        if (owner && detail::adopt_small_string_family(owner->root, owner->pages,
                *root, pages)) return batch_status::ok;
    }
    uint64_t previous = 0;
    for (const auto& page : pages) {
        const auto size = page.value.size();
        if (page.owner != key || page.action != update_action::write ||
            page.id <= previous || (!string_family && page.id != previous + 1) ||
            page.value.capacity() - size > std::max<size_t>(16, size / 4))
            return batch_status::unsupported;
        previous = page.id;
    }
    try {
        commit_state state(*this, roots, pages, true);
        state.adopt_pages = &pages;
        if (!state.gather() || !state.prepare()) return batch_status::error;
        state.publish();
        return batch_status::ok;
    } catch (const std::bad_alloc&) { return batch_status::error; }
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
