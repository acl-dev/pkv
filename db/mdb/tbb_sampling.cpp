#include "stdafx.h"
#include "mdb_tbb.h"
#if defined(HAS_TBB)
#include "common/set_sampling.h"
#include "db/db_watcher.h"
#include <typeinfo>

namespace pkv {
template<class Visit>
bool mdb_tbb::visit_indexes(const store& shard, size_t first, const Visit& visit) {
    // A shared visitor locks the stripe before reading the tagged slot.
    for (size_t word = first / index_word_bits; word < shard.scan_pages.size(); ++word) {
        const auto* page = shard.scan_pages[word].load(std::memory_order_acquire);
        if (!page) continue;
        auto bits = page->live.load(std::memory_order_acquire);
        if (word == first / index_word_bits)
            bits &= UINT64_MAX << (first % index_word_bits);
        while (bits) {
            const auto bit = __builtin_ctzll(bits);
            if (!visit(word * index_word_bits + bit, page->indexes[bit])) return false;
            bits &= bits - 1;
        }
    }
    return true;
}

template<class Visit>
db::scan_status mdb_tbb::walk_scan_page(unsigned long long cursor, size_t count,
    unsigned long long& next, const Visit& visit) {
    next = 0;
    const auto start = cursor / scan_buckets;
    if (start >= shard_count_) return scan_status::ok;
    if (!count) count = 1;
    try {
        const auto limit = static_cast<unsigned long long>(shard_count_) * scan_buckets;
        size_t first = static_cast<size_t>(cursor % scan_buckets);
        size_t visited = 0;
        for (size_t shard = static_cast<size_t>(start); shard < shard_count_;
                ++shard, first = 0) {
            const auto* table = shard_at(shard);
            if (!table || !table->size()) continue;
            bool valid = true;
            const bool complete = visit_indexes(*table, first,
                [&](size_t bucket, const scan_index& index) {
                    auto& mutex = scan_lock({shard, bucket});
                    valid = mutex.lock();
                    if (!valid) return false;
                    std::unique_lock<acl::fiber_mutex> guard(mutex, std::adopt_lock);
                    const auto size = index.size();
                    for (size_t i = 0; i < size; ++i) {
                        valid = visit(*index[i]);
                        if (!valid) return false;
                    }
                    visited += size;
                    if (visited < count) return true;
                    const auto position = static_cast<unsigned long long>(shard) *
                        scan_buckets + bucket + 1;
                    if (position < limit) next = position;
                    return false;
                });
            if (!valid) return scan_status::error;
            if (!complete) break;
        }
        return scan_status::ok;
    } catch (const std::bad_alloc&) {
        next = 0; return scan_status::error;
    }
}

db::scan_status mdb_tbb::dbscan_keys(unsigned long long cursor, size_t count,
    std::vector<std::string>& keys, unsigned long long& next) {
    keys.clear();
    const auto status = walk_scan_page(cursor, count, next,
        [&](const owner_value& entry) {
            keys.push_back(entry.first);
            return true;
        });
    if (status != scan_status::ok) keys.clear();
    return status;
}

db::scan_status mdb_tbb::dbscan_root_page(unsigned long long cursor, size_t count,
    unsigned long long& next, const scan_root_visitor& visit,
    const key_visitor& filter) {
    if ((watchers_ && watchers_->observes_reads()) || typeid(*this) != typeid(mdb_tbb))
        return scan_status::unsupported;
    return walk_scan_page(cursor, count, next, [&](const owner_value& entry) {
        if (filter && !filter(entry.first)) return true;
        auto& mutex = key_mutex(entry.first);
        // Writers take the key lock before the scan stripe. Never wait here.
        if (!mutex.trylock()) return visit(entry.first, nullptr);
        std::unique_lock<acl::fiber_mutex> guard(mutex, std::adopt_lock);
        return visit(entry.first, &entry.second.data.root);
    });
}

const mdb_tbb::owner_value* mdb_tbb::owner_at_rank(const store& shard, uint64_t rank) {
    // Exclusive admission protects existing TBB node addresses and scan sets.
    const owner_value* owner = nullptr;
    visit_indexes(shard, 0, [&](size_t, const scan_index& index) {
        const auto count = index.size();
        if (rank >= count) { rank -= count; return true; }
        owner = index[static_cast<size_t>(rank)];
        return false;
    });
    return owner;
}

db::scan_status mdb_tbb::dbsample_key(std::string& out, bool& found) {
    if (typeid(*this) != typeid(mdb_tbb)) return db::dbsample_key(out, found);
    const auto count = root_count_.load(std::memory_order_relaxed);
    if (!count) {
        sample_shard_bound_.store(0, std::memory_order_relaxed);
        sample_bound_misses_ = 0;
        return scan_status::ok;
    }
    const auto bound = sample_shard_bound_.load(std::memory_order_relaxed);
    if (bound && bound <= SIZE_MAX / shard_count_) {
        const auto capacity = bound * shard_count_;
        // All keys occupy one equally likely cell. Skip probes when most
        // cells are empty, as with small or highly uneven keyspaces.
        if (capacity / 4 <= count) {
            auto& rng = set_sampling_engine();
            std::uniform_int_distribution<size_t> cell(0, capacity - 1);
            for (unsigned attempt = 0; attempt < 4; ++attempt) {
                const auto pick = cell(rng);
                const auto* shard = shard_at(pick / bound);
                const auto rank = pick % bound;
                if (!shard || rank >= shard->size()) continue;
                const auto* owner = owner_at_rank(*shard, rank);
                if (!owner) return scan_status::error;
                out = owner->first;
                found = true;
                return scan_status::ok;
            }
        }
    }
    // Deletions and rollback can leave a high bound. Recompute it only
    // periodically so persistent skew does not add a second walk per sample.
    if (bound > 1 && ++sample_bound_misses_ == 64) {
        size_t largest = 0;
        for (size_t i = 0; i < shard_count_; ++i)
            if (const auto* shard = shard_at(i))
                largest = std::max(largest, shard->size());
        sample_shard_bound_.store(largest, std::memory_order_relaxed);
        sample_bound_misses_ = 0;
    }
    return db::dbsample_key(out, found);
}

db::scan_status mdb_tbb::dbkey_at_rank(uint64_t rank, std::string& out,
    bool& found) {
    for (size_t i = 0; i < shard_count_; ++i) {
        const auto* store = shard_at(i);
        if (!store) continue;
        const auto size = store->size();
        if (rank >= size) { rank -= size; continue; }
        const auto* owner = owner_at_rank(*store, rank);
        if (!owner) return scan_status::error;
        out = owner->first;
        found = true;
        return scan_status::ok;
    }
    return scan_status::ok;
}
db::scan_status mdb_tbb::dbvisit_keys(const key_visitor& visit) {
    for (size_t i = 0; i < shard_count_; ++i) {
        const auto* store = shard_at(i);
        if (!store || !store->size()) continue;
        if (!visit_indexes(*store, 0, [&](size_t, const scan_index& index) {
            for (size_t i = 0; i < index.size(); ++i)
                if (!visit(index[i]->first)) return false;
            return true;
        })) break;
    }
    return scan_status::ok;
}
db::scan_status mdb_tbb::dbvisit_root_records(const root_visitor& visit) {
    for (size_t i = 0; i < shard_count_; ++i) {
        const auto* store = shard_at(i);
        if (!store || !store->size()) continue;
        if (!visit_indexes(*store, 0, [&](size_t, const scan_index& index) {
            for (size_t i = 0; i < index.size(); ++i) {
                const auto& entry = *index[i];
                if (!visit(entry.first, entry.second.data.root)) return false;
            }
            return true;
        })) break;
    }
    return scan_status::ok;
}
} // namespace pkv
#endif
