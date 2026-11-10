#include "stdafx.h"
#include "mdb_tbb.h"
#if defined(HAS_TBB)
#include "coder/list_page.h"
#include "common/key_crc32.h"
#include "db/page_transaction.h"
#include "replacement_workspace.h"
#include <cstring>
#include <climits>
#include <stdexcept>
#include <utility>

namespace pkv {
namespace {
template<class T, class... Args>
T* load_or_create(std::atomic<T*>& slot, bool create, Args&&... args) {
    auto* value = slot.load(std::memory_order_acquire);
    if (value || !create) return value;
    std::unique_ptr<T> candidate(new T(std::forward<Args>(args)...));
    if (slot.compare_exchange_strong(value, candidate.get(),
            std::memory_order_acq_rel, std::memory_order_acquire))
        return candidate.release();
    return value;
}
}

const size_t mdb_tbb::scan_buckets;

mdb_tbb::store::store(owner_map& table) : owners(table) {
    for (auto& page : scan_pages) page.store(nullptr, std::memory_order_relaxed);
}

mdb_tbb::store::~store() {
    for (auto& page : scan_pages) delete page.load(std::memory_order_relaxed);
}

mdb_tbb::mdb_tbb(size_t count) : shard_count_(count ? count : 1) {
    if (shard_count_ > ULLONG_MAX / scan_buckets)
        throw std::length_error("too many oneTBB shards");
    stores_.reset(new std::atomic<store*>[shard_count_]);
    for (size_t i = 0; i < shard_count_; ++i)
        stores_[i].store(nullptr, std::memory_order_relaxed);
    for (auto& table : owner_tables_) table.store(nullptr, std::memory_order_relaxed);
    for (auto& lock : scan_locks_) lock.store(nullptr, std::memory_order_relaxed);
}

mdb_tbb::~mdb_tbb() {
    for (size_t i = 0; i < shard_count_; ++i)
        delete stores_[i].load(std::memory_order_relaxed);
    for (auto& table : owner_tables_) delete table.load(std::memory_order_relaxed);
    for (auto& lock : scan_locks_) delete lock.load(std::memory_order_relaxed);
}
bool mdb_tbb::dbopen(const char*) { return true; }

bool mdb_tbb::trim_idle_storage(uint64_t now_ms) {
    constexpr uint64_t idle_ms = 1000;
    if (root_count_.load(std::memory_order_relaxed)) return false;
    auto since = empty_since_ms_.load(std::memory_order_relaxed);
    if (!since) {
        if (std::any_of(owner_tables_.begin(), owner_tables_.end(), [](const auto& table) {
            return table.load(std::memory_order_relaxed) != nullptr;
        })) empty_since_ms_.compare_exchange_strong(since, now_ms, std::memory_order_relaxed);
        return false;
    }
    if (now_ms < since || now_ms - since < idle_ms) return false;

    std::array<std::unique_ptr<acl::fiber_mutex>, scan_lock_groups * scan_lock_count> locks;
    std::array<std::unique_ptr<owner_map>, owner_table_count> tables;
    std::vector<std::unique_ptr<store>> shards;
    try {
        shards.reserve(shard_count_);
    } catch (const std::bad_alloc&) {
        return false;
    }
    {
        // Drain readers and writers before detaching the indexes they borrow.
        execution_guard admission(true);
        if (root_count_.load(std::memory_order_relaxed) ||
            empty_since_ms_.load(std::memory_order_relaxed) != since) return false;
        for (size_t i = 0; i < shard_count_; ++i) {
            if (auto* shard = stores_[i].exchange(nullptr, std::memory_order_relaxed))
                shards.emplace_back(shard);
        }
        for (size_t i = 0; i < owner_tables_.size(); ++i)
            tables[i].reset(owner_tables_[i].exchange(nullptr, std::memory_order_relaxed));
        for (size_t i = 0; i < scan_locks_.size(); ++i)
            locks[i].reset(scan_locks_[i].exchange(nullptr, std::memory_order_relaxed));
        sample_shard_bound_.store(0, std::memory_order_relaxed);
        sample_bound_misses_ = 0;
        empty_since_ms_.store(0, std::memory_order_relaxed);
    }
    // Destroy detached shards before their owner tables and scan locks.
    return true;
}

mdb_tbb::location mdb_tbb::locate(const std::string& key) const {
    const auto hash = static_cast<size_t>(key_crc32(key.data(),
        key.size()));
    return {hash % shard_count_, (hash / shard_count_) % scan_buckets};
}

acl::fiber_mutex& mdb_tbb::scan_lock(const location& position) {
    return *scan_locks_[scan_stripe(position)].load(std::memory_order_acquire);
}

mdb_tbb::scan_index* mdb_tbb::index(const location& position, bool create) {
    auto* shard = shard_at(position.shard);
    if (!shard) {
        if (!create) return nullptr;
        auto* table = load_or_create(owner_tables_[position.shard % owner_table_count], true);
        shard = load_or_create(stores_[position.shard], true, *table);
    }
    auto* page = load_or_create(shard->scan_pages[position.bucket / index_word_bits],
        create);
    if (!page) return nullptr;
    // Initialize the stripe before a writer can publish the live bit.
    if (create) load_or_create(scan_locks_[scan_stripe(position)], true);
    return &page->indexes[position.bucket % index_word_bits];
}

void mdb_tbb::scan_index::prepare() {
    if (auto* many = overflow()) {
        many->prepare();
    } else if (value_) {
        std::unique_ptr<multiple> next(new multiple);
        next->publish(single());
        value_ = reinterpret_cast<uintptr_t>(next.release()) | overflow_tag;
    }
}

void mdb_tbb::scan_index::publish(pointer node) noexcept {
    if (auto* many = overflow()) {
        many->publish(node);
    } else {
        if (value_) std::abort();
        node->second.position = 0;
        value_ = reinterpret_cast<uintptr_t>(node);
    }
}

void mdb_tbb::scan_index::cancel() noexcept {
    if (auto* many = overflow()) {
        many->cancel();
        collapse(*many, 1);
    }
}

void mdb_tbb::scan_index::erase(pointer node) noexcept {
    if (auto* many = overflow()) {
        many->erase(node);
        // Keep a two-entry allocation while the bucket still has one key.
        collapse(*many, 0);
    } else {
        if (single() != node) std::abort();
        value_ = 0;
    }
}

void mdb_tbb::scan_index::collapse(multiple& many, size_t limit) noexcept {
    const auto count = many.size();
    if (count > limit) return;
    value_ = count ? reinterpret_cast<uintptr_t>(many[0]) : 0;
    delete &many;
}

void mdb_tbb::scan_index::multiple::prepare() {
    if (dense_) {
        dense_->prepare();
        return;
    }
    if (!inline_.back()) return;

    // Keep the inline entries intact until every allocation succeeds.
    std::unique_ptr<dense_index> next(new dense_index);
    for (const auto node : inline_) {
        next->prepare();
        next->publish(node);
    }
    next->prepare();
    dense_ = std::move(next);
    inline_.fill(nullptr);
}

void mdb_tbb::scan_index::multiple::publish(pointer node) noexcept {
    if (dense_) {
        dense_->publish(node);
        return;
    }
    const auto position = inline_size();
    if (position == inline_.size()) std::abort();
    node->second.position = position;
    inline_[position] = node;
}

void mdb_tbb::scan_index::multiple::cancel() noexcept {
    if (!dense_) return;
    dense_->cancel();
    shrink_dense(inline_.size());
}

void mdb_tbb::scan_index::multiple::erase(pointer node) noexcept {
    if (dense_) {
        dense_->erase(node);
        // Retain space when a bucket repeatedly moves between two and three keys.
        shrink_dense(1);
        return;
    }
    const auto count = inline_size();
    const auto position = node->second.position;
    if (position >= count || inline_[position] != node) std::abort();
    auto& last = inline_[count - 1];
    inline_[position] = last;
    last->second.position = position;
    last = nullptr;
}

void mdb_tbb::scan_index::multiple::shrink_dense(size_t limit) noexcept {
    const auto count = dense_->size();
    if (count > limit) return;
    for (size_t i = 0; i < count; ++i) inline_[i] = (*dense_)[i];
    dense_.reset();
}

void mdb_tbb::mark_scan_slot(store& shard, size_t bucket, bool live) noexcept {
    auto* page = shard.scan_pages[bucket / index_word_bits].load(std::memory_order_acquire);
    const auto bit = uint64_t{1} << (bucket % index_word_bits);
    if (live) page->live.fetch_or(bit, std::memory_order_release);
    else page->live.fetch_and(~bit, std::memory_order_release);
}

void mdb_tbb::erase_scan_key(store& shard, size_t bucket, scan_index& index,
    const owner_value* owner) noexcept {
    index.erase(owner);
    if (!index.size()) mark_scan_slot(shard, bucket, false);
    shard.owner_count.fetch_sub(1, std::memory_order_relaxed);
    root_count_.fetch_sub(1, std::memory_order_relaxed);
}

const mdb_tbb::owner_value* mdb_tbb::add_owner(store& shard, size_t bucket,
    scan_index& index, const std::string& key) {
    owner_map::accessor inserted;
    index.prepare();
    try {
        if (!shard.owners.insert(inserted, key)) {
            index.cancel();
            return nullptr;
        }
        const auto* canonical = &*inserted;
        index.publish(canonical);
        if (index.size() == 1) mark_scan_slot(shard, bucket, true);
        const auto size = shard.owner_count.fetch_add(1, std::memory_order_relaxed) + 1;
        auto bound = sample_shard_bound_.load(std::memory_order_relaxed);
        while (bound < size && !sample_shard_bound_.compare_exchange_weak(
            bound, size, std::memory_order_relaxed)) {}
        if (!root_count_.fetch_add(1, std::memory_order_relaxed))
            empty_since_ms_.store(0, std::memory_order_relaxed);
        return canonical;
    } catch (const std::bad_alloc&) {
        // A failed segment allocation can leave a published node in accessor.
        if (!inserted.empty()) shard.owners.erase(inserted);
        index.cancel();
        throw;
    }
}

mdb_tbb::record* mdb_tbb::find(const std::string& key,
    const owner_value** canonical) {
    owner_map::const_accessor value;
    const auto shard = locate(key).shard;
    const auto* table = owner_tables_[shard % owner_table_count].load(std::memory_order_acquire);
    if (!table || !table->find(value, key)) return nullptr;
    if (canonical) *canonical = &*value;
    return &value->second.data;
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
        auto* owner = find(key);
        std::string replacement(value);
        if (owner) {
            owner->root.swap(replacement);
            page_map previous;
            owner->pages.swap(previous);
            return true;
        }
        return insert_root(key, replacement);
    } catch (const std::bad_alloc&) { return false; }
}

bool mdb_tbb::insert_root(const std::string& key, std::string& value) {
    const auto position = locate(key);
    auto* keys = index(position, true);
    auto& mutex = scan_lock(position);
    if (!mutex.lock()) return false;
    std::unique_lock<acl::fiber_mutex> guard(mutex, std::adopt_lock);
    const auto* added = add_owner(*shard_at(position.shard), position.bucket,
        *keys, key);
    if (!added) return false;
    added->second.data.root.swap(value);
    return true;
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
    auto& shard = *shard_at(position.shard);
    erase_scan_key(shard, position.bucket, *keys, canonical);
    return shard.owners.erase(key);
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

mdb_tbb::page_reader mdb_tbb::record_reader(const record* owner) {
    const auto read = [](const void* raw, uint64_t id) -> const std::string* {
        const auto* record = static_cast<const mdb_tbb::record*>(raw);
        return record ? record->pages.get(id) : nullptr;
    };
    return page_reader(read, owner);
}

const std::string* mdb_tbb::borrow_root(const std::string& key, page_reader& reader) {
    // The caller holds the logical key lock. An existing reader must be
    // pinned for this same key, even if other keys rehash the shard.
    if (!reader.read_) reader = record_reader(find(key));
    const auto* owner = static_cast<const record*>(reader.context_);
    return owner ? &owner->root : nullptr;
}

bool mdb_tbb::dbbind_page_reader(const std::string& key, page_reader& reader) {
    // Preserve any overridden page lookup or inspection hooks.
    if (typeid(*this) != typeid(mdb_tbb)) return false;
    reader = record_reader(find(key));
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
                const auto status = replace_existing_page(root, page, owner);
                if (status != batch_status::unsupported) return status;
            } catch (const std::bad_alloc&) { return batch_status::error; }
        }
    }
    return commit(roots, pages, true);
}

db::batch_status mdb_tbb::replace_bound_page(const batch_entry& root,
    const page_change& page, const page_reader& reader, std::string* adopted) {
    return replace_existing_page(root, page,
        const_cast<record*>(static_cast<const record*>(reader.context_)), adopted);
}

db::batch_status mdb_tbb::edit_bound_zset_pages(const batch_entry& root,
    const std::vector<page_change>& pages, const page_reader& reader) {
    auto* owner = const_cast<record*>(static_cast<const record*>(reader.context_));
    return owner ? detail::commit_bound_zset_pages<false>(owner->root, owner->pages, root, pages) :
        batch_status::unsupported;
}

db::batch_status mdb_tbb::replace_bound_root(const batch_entry& root,
    const page_reader& reader) {
    // The existence check and publication share the logical key lock.
    // Keep the input bytes for expiry tracking and replication observers.
    auto* owner = const_cast<record*>(static_cast<const record*>(reader.context_));
    try {
        if (!owner) {
            std::string replacement(root.value);
            return insert_root(root.key, replacement) ? batch_status::ok : batch_status::error;
        }
        auto& stored = owner->root;
        if (stored.size() == root.value.size()) {
            if (!stored.empty()) std::memcpy(&stored[0], root.value.data(), stored.size());
        } else {
            std::string replacement(root.value);
            stored.swap(replacement);
        }
        if (root.clear_pages) {
            page_map retired;
            owner->pages.swap(retired);
        }
        return batch_status::ok;
    } catch (const std::bad_alloc&) { return batch_status::error; }
}

db::batch_status mdb_tbb::edit_bound_list_page(const batch_entry& root,
    const list_page_edit& edit, const page_reader& reader) {
    // The scope pins the owner under its logical key lock. Finish the
    // page edit before publishing the matching root.
    auto* owner = const_cast<record*>(static_cast<const record*>(reader.context_));
    auto* page = owner ? owner->pages.get(edit.id) : nullptr;
    if (!page || owner->root.size() != root.value.size()) return batch_status::error;
    if (list_page_codec::apply_edit(*page, edit) != list_page_status::ok)
        return batch_status::error;
    std::memcpy(&owner->root[0], root.value.data(), root.value.size());
    return batch_status::ok;
}

db::batch_status mdb_tbb::replace_existing_page(const batch_entry& root,
    const page_change& page, record* owner, std::string* adopted) {
    if (root.clear_pages && owner && !owner->pages.has_small_pages(1))
        return batch_status::unsupported;
    auto* stored = owner ? owner->pages.get(page.id) : nullptr;
    if (!stored || owner->root.size() != root.value.size())
        return batch_status::unsupported;
    try {
        // The transaction holds the logical key lock. Prepare any throwing
        // resize before publishing either record.
        if (adopted) {
            stored->swap(*adopted);
        } else if (stored->size() == page.value.size() &&
            !detail::needs_inline_buffer(stored->capacity(), page.value.size())) {
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
    // Exclusive admission drains mutations, including prepared batch rollback.
    out = root_count_.load(std::memory_order_relaxed);
    return count_status::ok;
}

bool mdb_tbb::dbscan(size_t, db_cursor&, std::vector<std::string>&, size_t) {
    return false;
}

} // namespace pkv
#endif
