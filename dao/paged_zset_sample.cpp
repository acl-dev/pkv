#include "stdafx.h"
#include "paged_zset.h"
#include "common/rank_sampling.h"
#include <algorithm>
#include <stdexcept>

namespace pkv {
namespace dao {

bool paged_zset::choose(zset_store& store, long long count,
    std::vector<zset_member>& out) {
    const auto size = store.metadata().elements;
    if (!size || !count) return true;
    const bool repeat = count < 0;
    uint64_t wanted = static_cast<uint64_t>(repeat ? -count : count);
    if (!repeat) wanted = std::min(wanted, size);
    if (wanted > out.max_size()) {
        read_status_ = read_status::sample_too_large; return false;
    }
    out.reserve(static_cast<size_t>(wanted));
    if (!repeat && wanted == size) {
        return store.visit([&](const std::string& member, double score) {
            out.push_back({member, score}); return true;
        });
    }
    rank_sampler ranks(size, repeat);
    for (uint64_t i = 0; i < wanted; ++i) {
        const auto rank = ranks.next();
        zset_member value;
        bool present;
        if (!store.select(rank, value, present)) return false;
        if (!present) {
            read_status_ = read_status::invalid_record; return false;
        }
        out.push_back(std::move(value));
    }
    return true;
}

bool paged_zset::sample(shared_db& db, const std::string& key, long long count,
    zset_sample_result& out) {
    reset(); out = zset_sample_result();
    if (count == LLONG_MIN) {
        read_status_ = read_status::invalid_integer; return false;
    }
    bool ok;
    try {
        ok = run(db, key, [&](zset_store& store) {
            out.found = store.exists();
            return choose(store, count, out.values);
        });
    } catch (const std::length_error&) {
        read_status_ = read_status::sample_too_large; ok = false;
    }
    if (!ok) out = zset_sample_result();
    return ok;
}

} // namespace dao
} // namespace pkv
