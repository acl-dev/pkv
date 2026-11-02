#include "stdafx.h"
#include "mdb.h"
#include "db/page_transaction.h"
#include "db/prepared_family.h"
#include "db/detached_pages.h"
#include <algorithm>

namespace pkv {
struct mdb::commit_state : prepared_batch {
    struct root_addition {
        root_map* bucket;
        const batch_entry* entry;
        store* shard;
    };
    struct page_addition { page_map* store; const page_change* entry; };
    struct owner_addition { page_family* family; const std::string* key; };
    struct page_reset {
        page_map* store;
        page_map previous;
        explicit page_reset(page_map* value) : store(value) {}
    };
    mdb& database;
    const std::vector<batch_entry>& roots;
    const std::vector<page_change>& pages;
    const bool preserve;
    std::vector<std::unique_lock<lock_t>> guards;
    std::vector<std::pair<std::string*, std::string>> replacements;
    std::vector<root_addition> root_additions;
    std::vector<page_addition> page_additions;
    std::vector<owner_addition> owner_additions;
    std::vector<page_reset> resets;
    size_t swapped = 0, inserted_roots = 0, inserted_pages = 0;
    bool done = false;
    commit_state(mdb& db, const std::vector<batch_entry>& r,
        const std::vector<page_change>& p, bool keep)
        : database(db), roots(r), pages(p), preserve(keep) {}
    ~commit_state() override { if (!done) rollback(); }
    void gather();
    void prepare();
    void publish() noexcept override;
    std::unique_ptr<prepared_transfer> prepare_family(
        prepared_batch& target) override;
    std::unique_ptr<prepared_transfer> prepare_family(const std::string& key,
        prepared_pages& source) override;
    void rollback() noexcept;
};

void mdb::commit_state::gather() {
    std::vector<size_t> shards;
    for (const auto& root : roots)
        if (root.action != update_action::keep)
            shards.push_back(database.locate(root.key).shard);
    for (const auto& page : pages)
        shards.push_back(database.locate(page.owner).shard);
    std::sort(shards.begin(), shards.end());
    shards.erase(std::unique(shards.begin(), shards.end()), shards.end());
    guards.reserve(shards.size());
    for (auto shard : shards) guards.emplace_back(*database.locks_[shard]);
}

void mdb::commit_state::prepare() {
    // Native locks remain held until publication or complete rollback.
    // Copy touched bytes only, never an entire owner's page map.
    if (preserve) {
        for (const auto& root : roots) {
            if (!root.clear_pages) continue;
            auto* family = database.find_page_family(
                database.locate(root.key), false);
            if (!family) continue;
            auto owner = family->find(root.key);
            if (owner != family->end()) {
                if (resets.empty()) resets.reserve(roots.size());
                resets.emplace_back(&owner->second);
            }
        }
        for (auto& reset : resets) {
            reset.store->swap(reset.previous); ++swapped;
        }
    }
    for (const auto& root : roots) {
        if (root.action != update_action::write) continue;
        const auto position = database.locate(root.key);
        auto* bucket = database.find_store(position, true);
        const auto it = bucket->find(root.key);
        if (it == bucket->end()) {
            if (root_additions.empty()) root_additions.reserve(roots.size());
            root_additions.push_back({bucket, &root,
                database.stores_[position.shard].get()});
        } else {
            if (replacements.empty())
                replacements.reserve(roots.size() + pages.size());
            replacements.emplace_back(&it->second.value, root.value);
        }
    }
    for (const auto& page : pages) {
        if (page.action != update_action::write) continue;
        auto* family = database.find_page_family(
            database.locate(page.owner), true);
        auto owner = family->find(page.owner);
        if (owner == family->end()) {
            // Reserve rollback bookkeeping before publishing a page owner.
            if (owner_additions.empty())
                owner_additions.reserve(std::min(roots.size(), pages.size()));
            owner = family->emplace(page.owner, page_map()).first;
            owner_additions.push_back({family, &page.owner});
        }
        auto& bucket = owner->second;
        const auto it = bucket.find(page.id);
        if (it == bucket.end()) {
            if (page_additions.empty()) page_additions.reserve(pages.size());
            page_additions.push_back({&bucket, &page});
        } else {
            if (replacements.empty())
                replacements.reserve(roots.size() + pages.size());
            replacements.emplace_back(&it->second, page.value);
        }
    }
    for (const auto& item : root_additions) {
        if (!database.add_root(*item.shard, *item.bucket, item.entry->key,
            item.entry->value)) throw std::bad_alloc();
        ++inserted_roots;
    }
    for (const auto& item : page_additions) {
        item.store->emplace(item.entry->id, item.entry->value);
        ++inserted_pages;
    }
}

void mdb::commit_state::rollback() noexcept {
    for (size_t i = 0; i < inserted_pages; ++i)
        page_additions[i].store->erase(page_additions[i].entry->id);
    for (size_t i = 0; i < inserted_roots; ++i) {
        auto& added = root_additions[i];
        database.erase_root(*added.shard, *added.bucket,
            added.bucket->find(added.entry->key));
    }
    for (const auto& owner : owner_additions) owner.family->erase(*owner.key);
    for (size_t i = 0; i < swapped; ++i)
        resets[i].store->swap(resets[i].previous);
}

std::unique_ptr<db::prepared_transfer> mdb::commit_state::prepare_family(
    prepared_batch& destination) {
    auto* target = dynamic_cast<commit_state*>(&destination);
    if (!target || target == this || roots.size() != 1 ||
        target->roots.size() != 1 || !pages.empty() ||
        !target->pages.empty() || !roots[0].found ||
        roots[0].action != update_action::erase ||
        target->roots[0].action != update_action::write ||
        !target->roots[0].clear_pages || !preserve || !target->preserve)
        return nullptr;
    auto* family = database.find_page_family(database.locate(roots[0].key),
        false);
    if (!family) return std::unique_ptr<prepared_transfer>(
        new prepared_family<page_map>(nullptr, nullptr));
    auto from = family->find(roots[0].key);
    if (from == family->end() || from->second.empty())
        return std::unique_ptr<prepared_transfer>(
            new prepared_family<page_map>(nullptr, nullptr));
    const auto& key = target->roots[0].key;
    auto* to_family = target->database.find_page_family(
        target->database.locate(key), true);
    auto to = to_family->find(key);
    if (to == to_family->end()) {
        target->owner_additions.reserve(target->owner_additions.size() + 1);
        to = to_family->emplace(key, page_map()).first;
        target->owner_additions.push_back({to_family, &key});
    }
    if (!to->second.empty()) return nullptr;
    return std::unique_ptr<prepared_transfer>(new prepared_family<page_map>(
        &from->second, &to->second));
}

std::unique_ptr<db::prepared_transfer> mdb::commit_state::prepare_family(
    const std::string& key, prepared_pages& source) {
    auto* family = dynamic_cast<detached_pages<page_map>*>(&source);
    auto root = std::lower_bound(roots.begin(), roots.end(), key,
        [](const batch_entry& a, const std::string& b) { return a.key < b; });
    if (!family || !preserve || !pages.empty() || root == roots.end() ||
        root->key != key || root->action != update_action::write ||
        !root->clear_pages) return nullptr;
    if (family->empty_after_publish()) return
        std::unique_ptr<prepared_transfer>(
            new prepared_family<page_map>(nullptr, nullptr));
    auto* owners = database.find_page_family(database.locate(key), true);
    auto target = owners->find(key);
    if (target == owners->end()) {
        if (owner_additions.empty())
            owner_additions.reserve(roots.size());
        target = owners->emplace(key, page_map()).first;
        owner_additions.push_back({owners, &root->key});
    }
    if (!target->second.empty()) return nullptr;
    return std::unique_ptr<prepared_transfer>(new prepared_family<page_map>(
        &family->values(), &target->second));
}

void mdb::commit_state::publish() noexcept {
    for (auto& replacement : replacements)
        replacement.first->swap(replacement.second);
    for (const auto& page : pages) {
        if (page.action != update_action::erase) continue;
        auto* family = database.find_page_family(
            database.locate(page.owner), false);
        if (!family) continue;
        const auto owner = family->find(page.owner);
        if (owner == family->end()) continue;
        owner->second.erase(page.id);
        if (owner->second.empty()) family->erase(owner);
    }
    for (const auto& root : roots) {
        if (root.action == update_action::erase) {
            const auto position = database.locate(root.key);
            auto* bucket = database.find_store(position, false);
            if (bucket) {
                const auto node = bucket->find(root.key);
                if (node != bucket->end()) database.erase_root(
                    *database.stores_[position.shard], *bucket, node);
            }
            database.erase_pages(root.key);
        } else if (root.action == update_action::write && !preserve)
            database.erase_pages(root.key);
    }
    done = true;
    guards.clear();
}

db::batch_status mdb::dbapply(const std::vector<batch_entry>& roots) {
    return commit(roots, {}, false);
}
db::batch_status mdb::dbapply_pages(const std::vector<batch_entry>& roots,
    const std::vector<page_change>& pages) {
    return commit(roots, pages, true);
}
db::batch_status mdb::dbprepare_pages(const std::vector<batch_entry>& roots,
    const std::vector<page_change>& pages, prepare_callback& ready) {
    return commit(roots, pages, true, &ready);
}
db::batch_status mdb::dbprepare_owned_pages(
    const std::vector<batch_entry>& roots,
    const std::vector<page_change>& pages,
    std::unique_ptr<prepared_batch>& out) {
    out.reset();
    try {
        std::unique_ptr<commit_state> state(
            new commit_state(*this, roots, pages, true));
        state->gather(); state->prepare();
        out = std::move(state); return batch_status::ok;
    } catch (const std::bad_alloc&) { return batch_status::error; }
}
db::batch_status mdb::commit(const std::vector<batch_entry>& roots,
    const std::vector<page_change>& pages, bool preserve_pages,
    prepare_callback* ready) {
    try {
        commit_state state(*this, roots, pages, preserve_pages);
        state.gather(); state.prepare();
        if (ready) return ready->prepared(state);
        state.publish(); return batch_status::ok;
    } catch (const std::bad_alloc&) { return batch_status::error; }
}
} // namespace pkv
