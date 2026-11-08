#include "stdafx.h"
#include "mdb.h"
#include "db/page_transaction.h"
#include "db/prepared_family.h"
#include "db/detached_pages.h"
#include "coder/list_page.h"
#include "coder/compact_zset.h"
#include "coder/compact_list_root.h"
#include "coder/compact_hash.h"
#include "replacement_workspace.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <cstring>
#include <new>

namespace pkv {
struct mdb::commit_state : prepared_batch {
    struct direct_copy {
        std::string* target;
        const std::string* source;
    };
    using direct_copies = std::array<direct_copy, 48>;
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
    std::vector<page_change>* adopt_pages = nullptr;
    page_map* adopt_target = nullptr;
    page_map* bound = nullptr;
    std::string** bound_root = nullptr;
    location single_position{};
    bool single_owner = false;
    std::unique_lock<lock_t> single_guard;
    std::vector<std::unique_lock<lock_t>> guards;
    // Most commits replace one root and at most one endpoint page.
    detail::replacement_workspace inline_replacements;
    size_t inline_count = 0;
    direct_copy* direct = nullptr;
    size_t direct_count = 0;
    std::vector<std::pair<std::string*, std::string>> replacements;
    std::vector<root_addition> root_additions;
    std::array<page_addition, 2> inline_additions;
    size_t inline_addition_count = 0;
    std::vector<page_addition> page_additions;
    std::vector<owner_addition> owner_additions;
    std::vector<page_reset> resets;
    size_t swapped = 0, inserted_roots = 0, inserted_pages = 0;
    bool done = false;
    commit_state(mdb& db, const std::vector<batch_entry>& r,
        const std::vector<page_change>& p, bool keep, bool recycle = false)
        : database(db), roots(r), pages(p), preserve(keep),
          inline_replacements(recycle) {}
    ~commit_state() override {
        if (!done) rollback();
        if (!replacements.empty()) inline_replacements.release_spill(
            replacements);
    }
    void add_page(page_map& store, const page_change& page) {
        if (inline_addition_count < inline_additions.size()) {
            inline_additions[inline_addition_count++] = {&store, &page};
            return;
        }
        if (page_additions.empty()) {
            page_additions.reserve(pages.size());
            page_additions.insert(page_additions.end(),
                inline_additions.begin(), inline_additions.end());
        }
        page_additions.push_back({&store, &page});
    }
    const page_addition* additions() const {
        return page_additions.empty() ? inline_additions.data() : page_additions.data();
    }
    size_t addition_count() const {
        return page_additions.empty() ? inline_addition_count : page_additions.size();
    }
    void gather();
    void prepare();
    bool prepare_replacement_family();
    void stage_replacement(std::string& target, const std::string& value,
        bool page = false);
    void publish() noexcept override;
    std::unique_ptr<prepared_transfer> prepare_family(
        prepared_batch& target) override;
    std::unique_ptr<prepared_transfer> prepare_family(const std::string& key,
        prepared_pages& source) override;
    void rollback() noexcept;
};

static bool compact_page_buffer(const std::string& bytes) {
    // Allow the allocator's small-string size-class rounding.
    return bytes.capacity() - bytes.size() <=
        std::max<size_t>(16, bytes.size() / 4);
}

static bool small_root_buffer(const std::string& bytes) {
    // Fixed metadata roots must not inherit a previous packed collection's
    // large allocation from the worker-local replacement workspace.
    return bytes.size() <= 64;
}

static bool seals_list_endpoint(const std::string& stored,
    const std::string& incoming) {
    if (incoming.size() != stored.size() ||
        incoming.size() < list_page_codec::page_header_bytes ||
        !list_page_codec::is_compact_page(incoming) ||
        incoming[3] != stored[3]) return false;
    constexpr char no_link[8] = {};
    return (stored.compare(16, 8, no_link, 8) == 0 &&
            incoming.compare(16, 8, no_link, 8) != 0) ||
        (stored.compare(24, 8, no_link, 8) == 0 &&
            incoming.compare(24, 8, no_link, 8) != 0);
}

void mdb::commit_state::gather() {
    if (bound) {
        // The page transaction has validated the sole owner and released
        // its unique page changes in ID order.
        assert(roots.size() == 1);
        single_owner = true;
        single_position = database.locate(roots[0].key);
        single_guard = std::unique_lock<lock_t>(*database.locks_[single_position.shard]);
        return;
    }
    uint64_t previous_id = 0;
    if (roots.size() == 1 && std::all_of(pages.begin(), pages.end(),
        [&](const page_change& page) {
            if (direct) {
                if (page.id <= previous_id) direct = nullptr;
                previous_id = page.id;
            }
            return page.owner == roots[0].key;
        })) {
        single_owner = true;
        if (roots[0].action != update_action::keep || !pages.empty()) {
            single_position = database.locate(roots[0].key);
            single_guard = std::unique_lock<lock_t>(
                *database.locks_[single_position.shard]);
        }
        return;
    }
    direct = nullptr;
    bound = nullptr;
    bound_root = nullptr;
    std::vector<size_t> shards;
    for (const auto& root : roots)
        if (root.action != update_action::keep)
            shards.push_back(database.locate(root.key).shard);
    const std::string* previous_owner = nullptr;
    for (const auto& page : pages) {
        if (previous_owner && *previous_owner == page.owner) continue;
        shards.push_back(database.locate(page.owner).shard);
        previous_owner = &page.owner;
    }
    std::sort(shards.begin(), shards.end());
    shards.erase(std::unique(shards.begin(), shards.end()), shards.end());
    guards.reserve(shards.size());
    for (auto shard : shards) guards.emplace_back(*database.locks_[shard]);
}

void mdb::commit_state::stage_replacement(std::string& target,
    const std::string& value, bool page) {
    if (direct && target.size() == value.size() &&
        target.capacity() - target.size() <= target.size() / 4 + 32) {
        assert(direct_count < roots.size() + pages.size());
        direct[direct_count++] = {&target, &value};
        // Preserve which of the remaining pages use spill's shrink policy.
        if (inline_count < inline_replacements.size()) ++inline_count;
        return;
    }
    if (inline_count < inline_replacements.size()) {
        inline_replacements.stage(inline_count, target, value,
            page ? seals_list_endpoint(target, value) : small_root_buffer(value));
        ++inline_count;
        return;
    }
    if (direct && value.size() < target.size()) {
        // A deferred overwrite can leave a cache entry from a different
        // workload. Shrinking a stored page must publish an exact-sized copy.
        if (replacements.empty()) replacements.reserve(roots.size() + pages.size());
        replacements.emplace_back(&target, value);
        return;
    }
    inline_replacements.stage_spill(replacements, target, value,
        roots.size() + pages.size());
}

bool mdb::commit_state::prepare_replacement_family() {
    if (!preserve || pages.size() < 32) return false;
    const auto& key = pages.front().owner;
    const auto root = std::find_if(roots.begin(), roots.end(),
        [&](const batch_entry& entry) { return entry.key == key; });
    if (root == roots.end() || !root->clear_pages ||
        root->action != update_action::write) return false;
    uint64_t previous = 0;
    for (const auto& page : pages) {
        if (page.owner != key || page.action != update_action::write ||
            page.id <= previous) return false;
        previous = page.id;
    }
    page_map replacement;
    replacement.prepare_empty(pages.begin(), pages.end(),
        [](const page_change& page) { return page.id; });
    const std::string empty;
    for (const auto& page : pages)
        replacement.emplace(page.id,
            adopt_pages && compact_page_buffer(page.value) ? empty : page.value);
    auto* family = database.find_page_family(database.locate(key), true);
    auto owner = family->find(key);
    if (owner == family->end()) {
        owner_additions.reserve(1);
        owner = family->emplace(key, page_map()).first;
        owner_additions.push_back({family, &key});
    }
    // The original map is already in resets. Rollback restores it or
    // removes the newly inserted owner, without tracking each new page.
    assert(owner->second.empty());
    owner->second.swap(replacement);
    if (adopt_pages) adopt_target = &owner->second;
    return true;
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
        if (bound_root && *bound_root) {
            stage_replacement(**bound_root, root.value);
            continue;
        }
        const auto position = single_owner ? single_position :
            database.locate(root.key);
        auto* bucket = database.find_store(position, true);
        const auto it = bucket->find(root.key);
        if (it == bucket->end()) {
            if (root_additions.empty()) root_additions.reserve(roots.size());
            root_additions.push_back({bucket, &root,
                database.stores_[position.shard].get()});
        } else {
            auto& stored = it->second.value;
            if (bound_root) *bound_root = &stored;
            stage_replacement(stored, root.value);
        }
    }
    const std::string* previous_owner = bound ? &roots[0].key : nullptr;
    page_map* bucket = bound;
    if (!prepare_replacement_family()) for (const auto& page : pages) {
        if (page.action != update_action::write) continue;
        if (!bucket || (!single_owner && *previous_owner != page.owner)) {
            const auto position = single_owner ? single_position :
                database.locate(page.owner);
            auto* family = database.find_page_family(position, true);
            auto owner = family->find(page.owner);
            if (owner == family->end()) {
                // Reserve rollback bookkeeping before publishing a page owner.
                if (owner_additions.empty())
                    owner_additions.reserve(std::min(roots.size(), pages.size()));
                owner = family->emplace(page.owner, page_map()).first;
                owner_additions.push_back({family, &page.owner});
            }
            previous_owner = &page.owner;
            bucket = &owner->second;
        }
        const auto it = bucket->get(page.id);
        if (!it) add_page(*bucket, page);
        else stage_replacement(*it, page.value, true);
    }
    for (const auto& item : root_additions) {
        if (!database.add_root(*item.shard, *item.bucket, item.entry->key,
            item.entry->value)) throw std::bad_alloc();
        ++inserted_roots;
    }
    const auto* end_additions = additions() + addition_count();
    for (auto item = additions(); item != end_additions; ++item) {
        if (item->store->empty()) {
            // A new family can size its index from the staged pages instead
            // of retaining the final bucket array from incremental growth.
            const auto end = std::find_if(item + 1, end_additions,
                [&](const page_addition& next) { return next.store != item->store; });
            item->store->prepare_empty(item, end,
                [](const page_addition& next) { return next.entry->id; });
        }
        item->store->emplace(item->entry->id, item->entry->value);
        ++inserted_pages;
    }
}

void mdb::commit_state::rollback() noexcept {
    const auto* inserted = additions();
    for (size_t i = 0; i < inserted_pages; ++i)
        inserted[i].store->erase(inserted[i].entry->id);
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
    if (adopt_target) {
        // All allocating preparation has succeeded. Until this point the
        // transaction still owns every staged payload for safe rollback.
        for (auto& page : *adopt_pages)
            if (compact_page_buffer(page.value))
                adopt_target->get(page.id)->swap(page.value);
    }
    for (size_t i = 0; i < inline_count; ++i)
        if (inline_replacements[i].first)
            inline_replacements[i].first->swap(inline_replacements[i].second);
    for (auto& replacement : replacements)
        replacement.first->swap(replacement.second);
    // All allocations succeeded. These synchronous inputs remain owned by
    // the caller and intact for observers; stored buffers keep their capacity.
    for (size_t i = 0; i < direct_count; ++i) {
        const auto& copy = direct[i];
        if (!copy.source->empty())
            std::memcpy(&(*copy.target)[0], copy.source->data(), copy.source->size());
    }
    // No prepared string pointers are dereferenced after this point.
    const auto* end_additions = additions() + addition_count();
    for (auto first = additions(); first != end_additions;) {
        auto last = first + 1;
        while (last != end_additions && last->store == first->store) ++last;
        first->store->promote_prefix(static_cast<size_t>(last - first));
        first->store->promote_tail((last - 1)->entry->id);
        first = last;
    }
    // A whole-key deletion below already releases all of its pages.
    if (!single_owner || roots[0].action != update_action::erase) {
        const std::string* previous_owner = nullptr;
        page_family* family = nullptr;
        page_family::iterator owner;
        for (const auto& page : pages) {
            if (page.action != update_action::erase) continue;
            if (!previous_owner || (!single_owner && *previous_owner != page.owner)) {
                previous_owner = &page.owner;
                const auto position = single_owner ? single_position :
                    database.locate(page.owner);
                family = database.find_page_family(position, false);
                if (family) {
                    owner = family->find(page.owner);
                    if (owner == family->end()) family = nullptr;
                }
            }
            if (!family) continue;
            owner->second.erase(page.id, true);
            if (owner->second.empty()) {
                family->erase(owner);
                if (family->empty()) database.prune_page_family(
                    single_owner ? single_position : database.locate(page.owner));
                family = nullptr;
            }
        }
    }
    for (const auto& root : roots) {
        if (root.action == update_action::erase) {
            const auto position = database.locate(root.key);
            database.erase_root(position, root.key);
            database.erase_pages(position, root.key);
        } else if (root.action == update_action::write && !preserve)
            database.erase_pages(single_owner ? single_position :
                database.locate(root.key), root.key);
    }
    done = true;
    guards.clear();
    if (single_guard.owns_lock()) single_guard.unlock();
}

db::batch_status mdb::dbapply(const std::vector<batch_entry>& roots) {
    return commit(roots, {}, false);
}
db::batch_status mdb::dbapply_pages(const std::vector<batch_entry>& roots,
    const std::vector<page_change>& pages) {
    if (roots.size() == 1 && pages.empty() &&
        roots[0].action == update_action::write) {
        try {
            if (replace_root(roots[0])) return batch_status::ok;
        } catch (const std::bad_alloc&) { return batch_status::error; }
    }
    if (roots.size() == 1 && pages.size() == 1 &&
        roots[0].action == update_action::write && !roots[0].clear_pages &&
        pages[0].action == update_action::write &&
        pages[0].owner == roots[0].key) {
        try {
            if (replace_page(roots[0], pages[0])) return batch_status::ok;
        } catch (const std::bad_alloc&) { return batch_status::error; }
    }
    return commit(roots, pages, true);
}

bool mdb::replace_root(const batch_entry& root) {
    // Own detached pages until after releasing the native lock. A root-only
    // replacement may clear a very large old family, as SET can do.
    page_map retired;
    const auto position = locate(root.key);
    std::unique_lock<lock_t> guard(*locks_[position.shard]);
    auto* bucket = find_store(position, false);
    if (!bucket) return false;
    const auto record = bucket->find(root.key);
    if (record == bucket->end()) return false;
    page_map* clearing = nullptr;
    if (root.clear_pages) {
        auto* family = find_page_family(position, false);
        if (family) {
            const auto owner = family->find(root.key);
            if (owner != family->end()) clearing = &owner->second;
        }
    }
    auto& stored = record->second.value;
    const auto size = root.value.size();
    const bool compact_collection = compact_list_root::matches(root.value) ||
        compact_hash::matches(root.value) || compact_set::matches(root.value);
    const bool resize_in_place = compact_collection && !root.clear_pages &&
        stored.capacity() >= size && stored.capacity() - size <= size / 4;
    if (stored.size() == size || resize_in_place) {
        if (stored.size() != size) stored.resize(size);
        if (clearing) clearing->swap(retired);
        if (!root.value.empty())
            std::memcpy(&stored[0], root.value.data(), root.value.size());
        return true;
    }
    // No native state changes until the only potentially throwing copy is
    // ready. Inputs remain intact for observers and prepared/owned APIs keep
    // using the general commit path.
    detail::replacement_workspace replacement(true);
    replacement.stage(0, stored, root.value,
        compact_collection || compact_zset::matches(root.value) ||
            small_root_buffer(root.value));
    if (clearing) clearing->swap(retired);
    stored.swap(replacement[0].second);
    guard.unlock();
    return true;
}

db::batch_status mdb::dbadopt_page(const batch_entry& root, page_change& page) {
    // Derived engines may intercept dbapply_pages, including fault injection.
    if (typeid(*this) != typeid(mdb) ||
        page.value.capacity() - page.value.size() > page.value.size() / 4)
        return batch_status::unsupported;
    return replace_page(root, page, &page.value) ? batch_status::ok :
        batch_status::unsupported;
}

db::batch_status mdb::dbadopt_page_family(const std::vector<batch_entry>& roots,
    std::vector<page_change>& pages) {
    if (typeid(*this) != typeid(mdb)) return batch_status::unsupported;
    try {
        commit_state state(*this, roots, pages, true, true);
        state.adopt_pages = &pages;
        state.gather();
        state.prepare();
        state.publish();
        return batch_status::ok;
    } catch (const std::bad_alloc&) { return batch_status::error; }
}

db::batch_status mdb::replace_bound_page(const batch_entry& root,
    page_change& page, const page_reader& reader, std::string*& bound_root,
    bool& adopt) {
    // The native reader was obtained under this owner's key lock and the
    // caller invalidates it before any commit that can remove the family.
    auto* pages = const_cast<page_map*>(static_cast<const page_map*>(reader.context_));
    adopt = adopt &&
        page.value.capacity() - page.value.size() <= page.value.size() / 4;
    if (adopt && replace_page(root, page, &page.value, pages, &bound_root))
        return batch_status::ok;
    adopt = false;
    return replace_page(root, page, nullptr, pages, &bound_root) ?
        batch_status::ok : batch_status::unsupported;
}

bool mdb::replace_page(const batch_entry& root, const page_change& page,
    std::string* adopted, page_map* bound, std::string** bound_root) {
    const auto position = locate(root.key);
    std::unique_lock<lock_t> guard(*locks_[position.shard]);
    // The scope pins the same root record until the logical key lock is
    // released, or the record is erased and the binding is cleared.
    auto* stored = bound_root ? *bound_root : nullptr;
    if (!stored) {
        auto* bucket = find_store(position, false);
        if (!bucket) return false;
        const auto record = bucket->find(root.key);
        if (record == bucket->end()) return false;
        stored = &record->second.value;
        if (bound_root) *bound_root = stored;
    }
    if (!bound) {
        auto* family = find_page_family(position, false);
        if (!family) return false;
        const auto owner = family->find(root.key);
        if (owner == family->end()) return false;
        bound = &owner->second;
    }
    const auto item = bound->get(page.id);
    if (!item) return false;
    auto& stored_root = *stored;
    auto& stored_page = *item;
    if (stored_root.size() == root.value.size()) {
        if (adopted) {
            stored_page.swap(*adopted);
        } else if (stored_page.size() == page.value.size()) {
            if (!page.value.empty())
                std::memcpy(&stored_page[0], page.value.data(),
                    page.value.size());
        } else {
            // Finish the potentially throwing resize before publication.
            detail::replacement_workspace replacement(true);
            replacement.stage(1, stored_page, page.value);
            stored_page.swap(replacement[1].second);
        }
        if (!root.value.empty())
            std::memcpy(&stored_root[0], root.value.data(), root.value.size());
        return true;
    }
    if (adopted) return false;
    // Prepare both copies before publication. Inputs remain available to
    // observers, and an allocation failure changes neither stored value.
    detail::replacement_workspace replacements(true);
    replacements.stage(0, stored_root, root.value, small_root_buffer(root.value));
    replacements.stage(1, stored_page, page.value);
    replacements[0].first->swap(replacements[0].second);
    replacements[1].first->swap(replacements[1].second);
    return true;
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
    prepare_callback* ready, page_map* bound, std::string** bound_root) {
    try {
        commit_state::direct_copies direct;
        std::unique_ptr<commit_state::direct_copy[]> wide;
        commit_state state(*this, roots, pages, preserve_pages, true);
        state.bound = bound;
        state.bound_root = bound_root;
        if (!ready && preserve_pages && roots.size() == 1 &&
            roots[0].action == update_action::write && !roots[0].clear_pages &&
            pages.size() > 1) {
            if (pages.size() < direct.size()) {
                state.direct = direct.data();
            } else if (bound && pages.size() <= 2049 &&
                roots[0].value.compare(0, 3, "PZR") == 0) {
                wide.reset(new (std::nothrow)
                    commit_state::direct_copy[pages.size() + 1]);
                state.direct = wide.get();
            }
        }
        state.gather(); state.prepare();
        if (ready) return ready->prepared(state);
        state.publish(); return batch_status::ok;
    } catch (const std::bad_alloc&) { return batch_status::error; }
}

db::batch_status mdb::apply_bound_pages(const std::vector<batch_entry>& roots,
    const std::vector<page_change>& pages, const page_reader& reader,
    std::string** bound_root) {
    // The logical key lock pins the page map and the stored root string.
    // Inserting other keys may rehash the maps without moving these values.
    auto* bound = const_cast<page_map*>(static_cast<const page_map*>(reader.context_));
    if (!bound) return batch_status::unsupported;
    return commit(roots, pages, true, nullptr, bound, bound_root);
}
} // namespace pkv
