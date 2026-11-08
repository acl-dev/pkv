#include "stdafx.h"
#include "mdb_tbb.h"
#include "db/key_count.h"
#if defined(HAS_TBB)
#include "common/key_crc32.h"
#include "db/page_transaction.h"
#include "replacement_workspace.h"
#include <cstring>
#include <climits>
#include <stdexcept>

namespace pkv {

const size_t mdb_tbb::scan_buckets;

mdb_tbb::store::store() {
    for (auto& entry : indexes) entry.store(nullptr, std::memory_order_relaxed);
    for (auto& lock : scan_locks) lock.store(nullptr, std::memory_order_relaxed);
}

mdb_tbb::store::~store() {
    for (auto& entry : indexes) delete entry.load(std::memory_order_relaxed);
    for (auto& lock : scan_locks) delete lock.load(std::memory_order_relaxed);
}

mdb_tbb::mdb_tbb(size_t count) {
    if (!count) count = 1;
    if (count > ULLONG_MAX / scan_buckets)
        throw std::length_error("too many oneTBB shards");
    stores_.reserve(count);
    for (size_t i = 0; i < count; ++i) stores_.emplace_back(new store);
}

mdb_tbb::~mdb_tbb() = default;
bool mdb_tbb::dbopen(const char*) { return true; }

mdb_tbb::location mdb_tbb::locate(const std::string& key) const {
    const auto hash = static_cast<size_t>(key_crc32(key.data(),
        key.size()));
    return {hash % stores_.size(), (hash / stores_.size()) % scan_buckets};
}

acl::fiber_mutex& mdb_tbb::scan_lock(const location& position) {
    return *stores_[position.shard]->scan_locks[position.bucket % scan_lock_count]
        .load(std::memory_order_acquire);
}

mdb_tbb::scan_index* mdb_tbb::index(const location& position, bool create) {
    auto& shard = *stores_[position.shard];
    auto& slot = shard.indexes[position.bucket];
    auto* value = slot.load(std::memory_order_acquire);
    if (value || !create) return value;
    // Publish the stripe before any bucket can expose a reference to it.
    auto& stripe = shard.scan_locks[position.bucket % scan_lock_count];
    auto* mutex = stripe.load(std::memory_order_acquire);
    if (!mutex) {
        std::unique_ptr<acl::fiber_mutex> candidate(new acl::fiber_mutex);
        if (stripe.compare_exchange_strong(mutex, candidate.get(),
                std::memory_order_acq_rel, std::memory_order_acquire))
            candidate.release();
    }
    std::unique_ptr<scan_index> candidate(new scan_index);
    if (slot.compare_exchange_strong(value, candidate.get(),
        std::memory_order_acq_rel, std::memory_order_acquire))
        return candidate.release();
    return value;
}

const mdb_tbb::owner_value* mdb_tbb::add_owner(store& shard,
    scan_index& index, const std::string& key, const record_ptr& value) {
    owner_map::accessor inserted;
    index.keys.prepare();
    try {
        if (!shard.owners.insert(inserted,
            owner_value(key, owner_entry(value)))) {
            index.keys.cancel(); return nullptr;
        }
        const auto* canonical = &*inserted;
        index.keys.publish(canonical);
        return canonical;
    } catch (const std::bad_alloc&) {
        // A failed segment allocation can leave a published node in accessor.
        if (!inserted.empty() && inserted->second.data == value)
            shard.owners.erase(inserted);
        index.keys.cancel(); throw;
    }
}

mdb_tbb::record_ptr mdb_tbb::find(const std::string& key,
    const owner_value** canonical) {
    owner_map::const_accessor value;
    auto& owners = stores_[locate(key).shard]->owners;
    if (!owners.find(value, key)) return nullptr;
    if (canonical) *canonical = &*value;
    return value->second.data;
}

db::lookup_status mdb_tbb::dblookup(const std::string& key,
    std::string& value) {
    try {
        const auto owner = find(key);
        if (!owner) return lookup_status::missing;
        // The public logical-key lock protects the record; no oneTBB accessor
        // remains held while payloads are copied or a callback may yield.
        value = owner->root;
        return lookup_status::found;
    } catch (const std::bad_alloc&) { return lookup_status::error; }
}

bool mdb_tbb::dbget(const std::string& key, std::string& value) {
    return dblookup(key, value) == lookup_status::found;
}

mdb_tbb::inline_status mdb_tbb::dbread_inline(const std::string& key,
    std::string& out, size_t offset, size_t limit) {
    // The public key lock protects root bytes after find releases its TBB
    // accessor, just as it does for dblookup and page transactions.
    const auto owner = find(key);
    if (!owner) return inline_status::missing;
    return decode_inline(owner->root, out, offset, limit);
}

bool mdb_tbb::dbset(const std::string& key, const std::string& value) {
    try {
        auto owner = find(key);
        if (owner) {
            std::string replacement(value);
            owner->root.swap(replacement);
            page_map previous;
            owner->pages.swap(previous);
            return true;
        }
        owner = std::make_shared<record>();
        owner->root = value;
        const auto position = locate(key);
        auto* keys = index(position, true);
        auto& mutex = scan_lock(position);
        if (!mutex.lock()) return false;
        std::unique_lock<acl::fiber_mutex> guard(mutex, std::adopt_lock);
        return add_owner(*stores_[position.shard], *keys, key, owner) !=
            nullptr;
    } catch (const std::bad_alloc&) { return false; }
}

bool mdb_tbb::dbdel(const std::string& key) {
    const owner_value* canonical = nullptr;
    if (!find(key, &canonical)) return false;
    const auto position = locate(key);
    auto* keys = index(position, false);
    if (!keys) return false;
    auto& mutex = scan_lock(position);
    if (!mutex.lock()) return false;
    std::unique_lock<acl::fiber_mutex> guard(mutex, std::adopt_lock);
    keys->keys.erase(canonical);
    return stores_[position.shard]->owners.erase(key);
}

db::lookup_status mdb_tbb::dblookup_page(const std::string& key, uint64_t id,
    std::string& value) {
    try {
        const auto owner = find(key);
        if (!owner) return lookup_status::missing;
        const auto* page = owner->pages.get(id);
        if (!page) return lookup_status::missing;
        value = *page;
        return lookup_status::found;
    } catch (const std::bad_alloc&) { return lookup_status::error; }
}

bool mdb_tbb::dbbind_page_reader(const std::string& key, page_reader& reader) {
    // Preserve any overridden page lookup or inspection hooks.
    if (typeid(*this) != typeid(mdb_tbb)) return false;
    const auto owner = find(key);
    // The transaction holds the logical key lock. The record and its page
    // map stay alive until publication, even if other keys rehash the shard.
    const auto read = [](const void* raw, uint64_t id) -> const std::string* {
        const auto* record = static_cast<const mdb_tbb::record*>(raw);
        return record ? record->pages.get(id) : nullptr;
    };
    reader = page_reader(read, owner.get());
    return true;
}

db::batch_status mdb_tbb::dbapply(const std::vector<batch_entry>& roots) {
    return commit(roots, {}, false);
}

db::batch_status mdb_tbb::dbapply_pages(const std::vector<batch_entry>& roots,
    const std::vector<page_change>& pages) {
    if (roots.size() == 1 && pages.size() == 1) {
        const auto& root = roots.front();
        const auto& page = pages.front();
        if (root.action == update_action::write && !root.clear_pages &&
            page.action == update_action::write && page.owner == root.key) {
            try {
                const auto owner = find(root.key);
                const auto status = replace_existing_page(root, page, owner.get());
                if (status != batch_status::unsupported) return status;
            } catch (const std::bad_alloc&) { return batch_status::error; }
        }
    }
    return commit(roots, pages, true);
}

db::batch_status mdb_tbb::replace_bound_page(const batch_entry& root,
    const page_change& page, const page_reader& reader) {
    return replace_existing_page(root, page,
        const_cast<record*>(static_cast<const record*>(reader.context_)));
}

db::batch_status mdb_tbb::replace_existing_page(const batch_entry& root,
    const page_change& page, record* owner) {
    auto* stored = owner ? owner->pages.get(page.id) : nullptr;
    if (!stored || owner->root.size() != root.value.size())
        return batch_status::unsupported;
    try {
        // The transaction holds the logical key lock. Prepare any throwing
        // resize before publishing either record.
        if (stored->size() == page.value.size()) {
            if (!page.value.empty())
                std::memcpy(&(*stored)[0], page.value.data(), page.value.size());
        } else {
            detail::replacement_workspace replacement(true);
            replacement.stage(1, *stored, page.value, true);
            stored->swap(replacement[1].second);
        }
        if (!root.value.empty())
            std::memcpy(&owner->root[0], root.value.data(), root.value.size());
        return batch_status::ok;
    } catch (const std::bad_alloc&) { return batch_status::error; }
}

db::count_status mdb_tbb::dbkey_count(uint64_t& out) {
    uint64_t count = 0;
    for (const auto& store : stores_)
        if (!add_key_count(store->owners.size(), count))
            return count_status::error;
    out = count; return count_status::ok;
}

db::scan_status mdb_tbb::dbscan_keys(unsigned long long cursor, size_t count,
    std::vector<std::string>& keys, unsigned long long& next) {
    keys.clear(); next = 0;
    if (cursor / scan_buckets >= stores_.size()) return scan_status::ok;
    if (!count) count = 1;
    try {
        const auto limit = static_cast<unsigned long long>(stores_.size()) *
            scan_buckets;
        while (cursor < limit) {
            const location position = {static_cast<size_t>(cursor /
                scan_buckets), static_cast<size_t>(cursor % scan_buckets)};
            auto* part = index(position, false);
            if (part) {
                auto& mutex = scan_lock(position);
                if (!mutex.lock()) {
                    keys.clear(); return scan_status::error;
                }
                std::unique_lock<acl::fiber_mutex> guard(mutex,
                    std::adopt_lock);
                for (size_t i = 0; i < part->keys.size(); ++i)
                    keys.push_back(part->keys[i]->first);
            }
            ++cursor;
            if (keys.size() >= count) break;
        }
        next = cursor < limit ? cursor : 0;
        return scan_status::ok;
    } catch (const std::bad_alloc&) {
        keys.clear(); next = 0; return scan_status::error;
    }
}

bool mdb_tbb::dbscan(size_t, db_cursor&, std::vector<std::string>&, size_t) {
    return false;
}

} // namespace pkv
#endif
