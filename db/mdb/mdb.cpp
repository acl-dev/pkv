#include "stdafx.h"
#include <algorithm>
#include <new>
#include <climits>
#include <stdexcept>
#include "mdb.h"
#include "db/key_count.h"
#include "db/page_transaction.h"

namespace pkv {

mdb::mdb(size_t count) {
    if (count == 0) count = 1;
    if (count > ULLONG_MAX / scan_buckets) throw
        std::length_error("too many memory shards");
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
    const auto hash = static_cast<size_t>(acl_hash_crc32(key.data(),
        key.size()));
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
            if (!add_root(*stores_[position.shard], *bucket, key, value))
                return false;
        }
        else it->second.value = value;
        erase_pages(key);
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

mdb::inline_status mdb::dbread_inline(const std::string& key, int64_t now,
    std::string& out) {
    const auto position = locate(key);
    std::lock_guard<lock_t> guard(*locks_[position.shard]);
    auto* bucket = find_store(position, false);
    if (!bucket) return inline_status::missing;
    const auto it = bucket->find(key);
    if (it == bucket->end()) return inline_status::missing;
    return decode_inline(it->second.value, now, out);
}

bool mdb::dbdel(const std::string& key) {
    const auto position = locate(key);
    std::lock_guard<lock_t> guard(*locks_[position.shard]);
    auto* bucket = find_store(position, false);
    bool erased = false;
    if (bucket) {
        const auto node = bucket->find(key);
        if (node != bucket->end()) {
            erase_root(*stores_[position.shard], *bucket, node);
            erased = true;
        }
    }
    erase_pages(key);
    return erased;
}

mdb::page_family* mdb::find_page_family(const location& position,
    bool create) {
    auto& family = stores_[position.shard]->pages[position.bucket];
    if (!family && create) {
        family.reset(new page_family);
    }
    return family.get();
}

void mdb::erase_pages(const std::string& owner) {
    auto* family = find_page_family(locate(owner), false);
    if (family) {
        family->erase(owner);
    }
}

db::lookup_status mdb::dblookup_page(const std::string& owner, uint64_t id,
    std::string& value) {
    const auto position = locate(owner);
    std::lock_guard<lock_t> guard(*locks_[position.shard]);
    const auto* family = find_page_family(position, false);
    if (!family) {
        return lookup_status::missing;
    }
    const auto record = family->find(owner);
    if (record == family->end()) {
        return lookup_status::missing;
    }
    const auto page = record->second.find(id);
    if (page == record->second.end()) {
        return lookup_status::missing;
    }
    value = page->second;
    return lookup_status::found;
}

db::count_status mdb::dbkey_count(uint64_t& out) {
    uint64_t count = 0;
    for (size_t shard = 0; shard < stores_.size(); ++shard) {
        std::lock_guard<lock_t> guard(*locks_[shard]);
        if (!add_key_count(stores_[shard]->keys.size(), count))
            return count_status::error;
    }
    out = count; return count_status::ok;
}

db::scan_status mdb::dbscan_keys(unsigned long long cursor, size_t count,
        std::vector<std::string>& keys, unsigned long long& next) {
    keys.clear(); next = 0;
    const auto shard_number = cursor / scan_buckets;
    if (shard_number >= stores_.size()) return scan_status::ok;
    if (!count) count = 1;
    try {
        size_t shard = static_cast<size_t>(shard_number);
        size_t bucket = static_cast<size_t>(cursor % scan_buckets);
        while (shard < stores_.size()) {
            {
                std::lock_guard<lock_t> guard(*locks_[shard]);
                while (bucket < scan_buckets) {
                    const auto* part = stores_[shard]->buckets[bucket].get();
                    if (part) for (const auto& item : *part)
                        keys.push_back(item.first);
                    ++bucket;
                    if (keys.size() >= count) break;
                }
            }
            if (bucket == scan_buckets) { ++shard; bucket = 0; }
            if (keys.size() >= count) break;
        }
        next = shard < stores_.size() ? static_cast<unsigned long long>(shard) *
            scan_buckets + bucket : 0;
        return scan_status::ok;
    } catch (const std::bad_alloc&) {
        keys.clear(); next = 0; return scan_status::error;
    }
}

bool mdb::dbscan(size_t, db_cursor&, std::vector<std::string>&, size_t) { return
    false; }

} // namespace pkv
