#include "stdafx.h"
#include "mdb.h"
#include "db/key_count.h"
#include "common/set_sampling.h"
#include <typeinfo>

namespace pkv {
template<class Visit>
bool mdb::visit_key_shards(const Visit& visit) {
    // Exclusive admission drains all mutations before reading the bitmap.
    for (size_t word = 0; word < occupied_key_shards_.size(); ++word) {
        auto bits = occupied_key_shards_[word].load(std::memory_order_relaxed);
        while (bits) {
#if defined(__GNUC__) || defined(__clang__)
            const auto bit = __builtin_ctzll(bits);
#else
            size_t bit = 0;
            for (auto low = bits; !(low & 1); low >>= 1) ++bit;
#endif
            const auto shard = word * shard_word_bits + bit;
            if (!visit(*stores_[shard])) return false;
            bits &= bits - 1;
        }
    }
    for (size_t shard = tracked_key_shards; shard < stores_.size(); ++shard)
        if (!visit(*stores_[shard])) return false;
    return true;
}

db::scan_status mdb::dbsample_key(std::string& out, bool& found) {
    if (typeid(*this) != typeid(mdb)) return db::dbsample_key(out, found);
    const auto bound = sample_shard_bound_.load(std::memory_order_relaxed);
    if (!bound) return scan_status::ok;
    auto& rng = set_sampling_engine();
    if (bound <= SIZE_MAX / stores_.size()) {
        // Each real key occupies one equally likely cell. Empty cells are
        // rejected, so sparse and uneven shards do not bias the result.
        std::uniform_int_distribution<size_t> cell(0, bound * stores_.size() - 1);
        for (unsigned attempt = 0; attempt < 4; ++attempt) {
            const auto pick = cell(rng);
            const auto& keys = stores_[pick / bound]->keys;
            const auto slot = pick % bound;
            if (slot < keys.size()) {
                out = keys[slot]->first;
                found = true; return scan_status::ok;
            }
        }
    }
    // Exclusive admission lets the fallback count without shard locks and
    // lower a bound left high by deletions or a rolled-back insertion.
    uint64_t count = 0;
    size_t largest = 0;
    const bool counted = visit_key_shards([&](const store& shard) {
        const auto size = shard.keys.size();
        largest = std::max(largest, size);
        return add_key_count(size, count);
    });
    if (!counted) return scan_status::error;
    sample_shard_bound_.store(largest, std::memory_order_relaxed);
    if (!count) return scan_status::ok;
    const auto rank = std::uniform_int_distribution<uint64_t>(0, count - 1)(rng);
    const auto status = dbkey_at_rank(rank, out, found);
    return status == scan_status::ok && !found ? scan_status::error : status;
}

db::scan_status mdb::dbkey_at_rank(uint64_t rank, std::string& out,
    bool& found) {
    // Exclusive admission stabilizes borrowed hash-node addresses.
    visit_key_shards([&](const store& shard) {
        const auto size = shard.keys.size();
        if (rank >= size) { rank -= size; return true; }
        out = shard.keys[static_cast<size_t>(rank)]->first;
        found = true; return false;
    });
    return scan_status::ok;
}
db::scan_status mdb::dbvisit_keys(const key_visitor& visit) {
    for (const auto& shard : stores_)
        for (size_t i = 0; i < shard->keys.size(); ++i)
            if (!visit(shard->keys[i]->first)) return scan_status::ok;
    return scan_status::ok;
}
db::scan_status mdb::dbvisit_root_records(const root_visitor& visit) {
    for (const auto& shard : stores_)
        for (size_t i = 0; i < shard->keys.size(); ++i) {
            const auto& entry = *shard->keys[i];
            if (!visit(entry.first, entry.second.value)) return scan_status::ok;
        }
    return scan_status::ok;
}
} // namespace pkv
