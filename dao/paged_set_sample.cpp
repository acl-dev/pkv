#include "stdafx.h"
#include "paged_set.h"
#include "common/rank_sampling.h"
#include <algorithm>
#include <stdexcept>

namespace pkv {
namespace dao {

bool paged_set::choose(map_store& store, long long count,
    std::vector<std::string>& out) {
    const uint64_t size = store.metadata().elements;
    if (!size || !count) return true;
    uint64_t wanted = static_cast<uint64_t>(count < 0 ? -count : count);
    const bool repeat = count < 0;
    if (!repeat) wanted = std::min(wanted, size);
    if (wanted > out.max_size()) {
        read_status_ = read_status::sample_too_large; return false;
    }
    out.reserve(static_cast<size_t>(wanted));
    if (!repeat && wanted == size) {
        return store.visit([&](const std::string& member, const std::string&) {
            out.push_back(member); return true;
        }, false);
    }
    // Sparse Fisher-Yates shuffles numeric ranks, so a small sample needs
    // O(wanted) scratch space and never copies all members or their pointers.
    rank_sampler ranks(size, repeat);
    for (uint64_t i = 0; i < wanted; ++i) {
        const auto rank = ranks.next();
        std::string member;
        bool present;
        if (!store.select(rank, member, present)) return false;
        if (!present) {
            read_status_ = read_status::invalid_record; return false;
        }
        out.push_back(std::move(member));
    }
    return true;
}

bool paged_set::sample(shared_db& db, const std::string& key,
    long long count, bool remove, set_sample_result& out) {
    reset(); out = set_sample_result();
    if (count == LLONG_MIN || (remove && count < 0)) {
        read_status_ = read_status::invalid_integer; return false;
    }
    bool ok;
    try {
        ok = run(db, key, [&](map_store& store) {
            out.found = store.exists();
            if (!choose(store, count, out.values)) return false;
            if (remove) {
                for (const auto& member : out.values) {
                    bool removed;
                    if (!store.erase(member, removed)) return false;
                    if (!removed) {
                        read_status_ = read_status::invalid_record;
                        return false;
                    }
                }
            }
            return true;
        });
    } catch (const std::length_error&) {
        read_status_ = read_status::sample_too_large; ok = false;
    }
    if (!ok) out = set_sample_result();
    return ok;
}

} // namespace dao
} // namespace pkv
