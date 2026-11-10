#include "stdafx.h"
#include <functional>
#include <algorithm>
#include <new>
#include <climits>
#include <stdexcept>
#include <typeinfo>
#include "mdb.h"
#include "db/page_transaction.h"

namespace pkv {

mdb::mdb(size_t count) {
    if (count == 0) count = 1;
    if (count > ULLONG_MAX / scan_buckets) throw
        std::length_error("too many memory shards");
    for (auto& word : occupied_key_shards_) word.store(0, std::memory_order_relaxed);
    locks_.reserve(count);
    stores_.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        locks_.emplace_back(new lock_t);
        stores_.emplace_back(new store);
    }
}
mdb::~mdb() = default;
bool mdb::dbopen(const char*) { return true; }

mdb::location mdb::locate(const std::string& key) const {
    // This hash only selects in-memory shards and scan buckets.
    // Encoded page IDs and logical map-tree buckets keep their format.
    const auto hash = std::hash<std::string>()(key);
    return {hash % stores_.size(), (hash / stores_.size()) % scan_buckets};
}

mdb::root_map* mdb::find_store(const location& position, bool create) {
    auto& bucket = stores_[position.shard]->buckets[position.bucket];
    if (!bucket && create) bucket.reset(new root_map);
    return bucket.get();
}

bool mdb::dbset(const std::string& key, const std::string& value) {
    const auto position = locate(key);
    std::lock_guard<lock_t> guard(*locks_[position.shard]);
    try {
        auto* bucket = find_store(position, true);
        auto it = bucket->find(key);
        if (it == bucket->end()) {
            if (!add_root(position.shard, *bucket, key, value))
                return false;
        }
        else it->second.value = value;
        erase_pages(position, key);
        return true;
    } catch (const std::bad_alloc&) { return false; }
}

bool mdb::dbget(const std::string& key, std::string& value) {
    const auto position = locate(key);
    std::lock_guard<lock_t> guard(*locks_[position.shard]);
    auto* bucket = find_store(position, false);
    if (!bucket) return false;
    auto it = bucket->find(key);
    if (it == bucket->end()) return false;
    value = it->second.value;
    return true;
}

mdb::lookup_status mdb::dbprobe_root(const std::string& key,
    std::string& scratch) {
    if (typeid(*this) != typeid(mdb)) return db::dbprobe_root(key, scratch);
    const auto position = locate(key);
    std::lock_guard<lock_t> guard(*locks_[position.shard]);
    const auto* bucket = find_store(position, false);
    return bucket && bucket->find(key) != bucket->end() ?
        lookup_status::found : lookup_status::missing;
}

mdb::inline_status mdb::dbread_inline(const std::string& key,
    std::string& out, size_t offset, size_t limit) {
    const auto position = locate(key);
    std::lock_guard<lock_t> guard(*locks_[position.shard]);
    auto* bucket = find_store(position, false);
    if (!bucket) return inline_status::missing;
    const auto it = bucket->find(key);
    if (it == bucket->end()) return inline_status::missing;
    return decode_inline(it->second.value, out, offset, limit);
}

mdb::inline_status mdb::read_inline_hashed(const std::string& key,
    std::string& out, size_t offset, size_t limit, size_t hash) {
    const location position{hash % stores_.size(),
        (hash / stores_.size()) % scan_buckets};
    std::lock_guard<lock_t> guard(*locks_[position.shard]);
    auto* bucket = find_store(position, false);
    if (!bucket) return inline_status::missing;
    const auto it = bucket->find(key);
    if (it == bucket->end()) return inline_status::missing;
    return decode_inline(it->second.value, out, offset, limit);
}

const std::string* mdb::borrow_root(const std::string& key, size_t hash) {
    const location position{hash % stores_.size(),
        (hash / stores_.size()) % scan_buckets};
    std::lock_guard<lock_t> guard(*locks_[position.shard]);
    const auto* bucket = find_store(position, false);
    if (!bucket) return nullptr;
    const auto it = bucket->find(key);
    // The caller holds the logical key lock. Rehashing keeps nodes stable.
    return it == bucket->end() ? nullptr : &it->second.value;
}

bool mdb::dbdel(const std::string& key) {
    const auto position = locate(key);
    std::lock_guard<lock_t> guard(*locks_[position.shard]);
    const bool erased = erase_root(position, key);
    erase_pages(position, key);
    return erased;
}

mdb::page_family* mdb::find_page_family(size_t shard, bool create) {
    auto& pages = stores_[shard]->pages;
    if (!pages && create) pages.reset(new page_family);
    return pages.get();
}

void mdb::erase_pages(const location& position, const std::string& owner) {
    auto* family = find_page_family(position.shard, false);
    if (family && family->erase(owner)) prune_page_family(position.shard);
}

void mdb::prune_page_family(size_t shard) noexcept {
    auto& pages = stores_[shard]->pages;
    if (!pages) return;
    if (pages->empty()) {
        pages.reset();
        return;
    }
    const auto buckets = pages->bucket_count();
    if (buckets >= 64 && pages->size() <= buckets / 4) {
        // Rehashing preserves page-map addresses pinned by logical key locks.
        try { pages->rehash(pages->size() * 2); }
        catch (const std::bad_alloc&) { /* Contraction is optional. */ }
    }
}

db::lookup_status mdb::dblookup_page(const std::string& owner, uint64_t id,
    std::string& value) {
    const auto position = locate(owner);
    std::lock_guard<lock_t> guard(*locks_[position.shard]);
    const auto* family = find_page_family(position.shard, false);
    if (!family) {
        return lookup_status::missing;
    }
    const auto record = family->find(owner);
    if (record == family->end()) {
        return lookup_status::missing;
    }
    const auto page = record->second.get(id);
    if (!page) {
        return lookup_status::missing;
    }
    value = *page;
    return lookup_status::found;
}

bool mdb::dbborrows_pages() const {
    // A subclass may customize the legacy lookup hook. Preserve that hook
    // unless it explicitly opts in to the synchronous inspection contract.
    return typeid(*this) == typeid(mdb);
}

db::lookup_status mdb::dbinspect_page(const std::string& owner, uint64_t id,
    std::string& scratch, page_inspection& inspection) {
    if (!dbborrows_pages())
        return db::dbinspect_page(owner, id, scratch, inspection);
    const auto position = locate(owner);
    std::lock_guard<lock_t> guard(*locks_[position.shard]);
    const auto* family = find_page_family(position.shard, false);
    if (!family) return lookup_status::missing;
    const auto record = family->find(owner);
    if (record == family->end()) return lookup_status::missing;
    const auto page = record->second.get(id);
    if (!page) return lookup_status::missing;
    // Holding the shard lock pins the string for this nonreentrant decoder.
    inspection.consume(*page);
    return lookup_status::found;
}

bool mdb::dbbind_page_reader(const std::string& owner, page_reader& reader) {
    // Subclasses retain their existing per-page lookup/inspection hooks.
    if (typeid(*this) != typeid(mdb)) return false;
    const auto position = locate(owner);
    std::lock_guard<lock_t> guard(*locks_[position.shard]);
    const auto* family = find_page_family(position.shard, false);
    const page_map* pages = nullptr;
    if (family) {
        const auto record = family->find(owner);
        if (record != family->end()) pages = &record->second;
    }
    // Like dbvisit_pages, the logical key lock pins this mapped object even
    // when other owners rehash the containing family. Writes to this owner
    // remain staged until after the page transaction's final read.
    auto read = [](const void* raw, uint64_t id) -> const std::string* {
        const auto* records = static_cast<const page_map*>(raw);
        return records ? records->get(id) : nullptr;
    };
    reader = page_reader(read, pages);
    return true;
}

bool mdb::reader_has_more_pages(const page_reader& reader, size_t count) {
    const auto* pages = static_cast<const page_map*>(reader.context_);
    return pages && pages->size() > count;
}

db::count_status mdb::dbkey_count(uint64_t& out) {
    // Exclusive admission drains mutations, including prepared batch rollback.
    out = root_count_.load(std::memory_order_relaxed);
    return count_status::ok;
}

template<class Visit>
db::scan_status mdb::walk_root_page(unsigned long long cursor, size_t count,
    unsigned long long& next, const Visit& visit) {
    next = 0;
    const auto shard_number = cursor / scan_buckets;
    if (shard_number >= stores_.size()) return scan_status::ok;
    if (!count) count = 1;
    try {
        size_t shard = static_cast<size_t>(shard_number);
        size_t bucket = static_cast<size_t>(cursor % scan_buckets);
        size_t visited = 0;
        while (shard < stores_.size()) {
            {
                std::lock_guard<lock_t> guard(*locks_[shard]);
                while (bucket < scan_buckets) {
                    const auto* part = stores_[shard]->buckets[bucket].get();
                    if (part) {
                        for (const auto& item : *part)
                            if (!visit(item.first, item.second.value))
                                return scan_status::error;
                        visited += part->size();
                    }
                    ++bucket;
                    if (visited >= count) break;
                }
            }
            if (bucket == scan_buckets) { ++shard; bucket = 0; }
            if (visited >= count) break;
        }
        next = shard < stores_.size() ? static_cast<unsigned long long>(shard) *
            scan_buckets + bucket : 0;
        return scan_status::ok;
    } catch (const std::bad_alloc&) {
        return scan_status::error;
    }
}

db::scan_status mdb::dbscan_keys(unsigned long long cursor, size_t count,
    std::vector<std::string>& keys, unsigned long long& next) {
    keys.clear();
    const auto status = walk_root_page(cursor, count, next,
        [&](const std::string& key, const std::string&) {
            keys.push_back(key);
            return true;
        });
    if (status != scan_status::ok) keys.clear();
    return status;
}

db::scan_status mdb::dbscan_root_page(unsigned long long cursor, size_t count,
    unsigned long long& next, const scan_root_visitor& visit,
    const key_visitor& filter) {
    if (typeid(*this) != typeid(mdb)) return scan_status::unsupported;
    return walk_root_page(cursor, count, next,
        [&](const std::string& key, const std::string& bytes) {
            return (filter && !filter(key)) || visit(key, &bytes);
        });
}

bool mdb::dbscan(size_t, db_cursor&, std::vector<std::string>&, size_t) { return
    false; }

} // namespace pkv
