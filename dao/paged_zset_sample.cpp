#include "stdafx.h"
#include "paged_zset.h"
#include "coder/compact_zset.h"
#include "common/rank_sampling.h"
#include <algorithm>
#include <stdexcept>

namespace pkv {
namespace dao {

namespace {

template<class Select, class Visit>
bool sample_members(uint64_t size, long long count,
    std::vector<zset_member>& out, bool& too_large, Select select, Visit visit) {
    if (!size || !count) return true;
    const bool repeat = count < 0;
    const auto requested = static_cast<uint64_t>(repeat ? -count : count);
    const auto wanted = repeat ? requested : std::min(requested, size);
    if (wanted > out.max_size()) { too_large = true; return false; }
    out.reserve(static_cast<size_t>(wanted));
    if (!repeat && wanted == size) return visit();
    rank_sampler ranks(size, repeat);
    for (uint64_t i = 0; i < wanted; ++i)
        if (!select(ranks.next())) return false;
    return true;
}

} // namespace

bool paged_zset::sample(shared_db& db, const std::string& key, long long count,
    zset_sample_result& out) {
    reset(); out = zset_sample_result();
    if (count == LLONG_MIN) {
        read_status_ = read_status::invalid_integer; return false;
    }
    bool ok;
    bool too_large = false;
    auto tree = [&](zset_store& store) {
        out.found = store.exists();
        return sample_members(store.metadata().elements, count, out.values,
            too_large, [&](uint64_t rank) {
                zset_member value;
                bool present;
                if (!store.select(rank, value, present)) return false;
                if (!present) {
                    read_status_ = read_status::invalid_record;
                    return false;
                }
                out.values.push_back(std::move(value));
                return true;
            }, [&] {
                return store.visit([&](const std::string& member, double score) {
                    out.values.push_back({member, score});
                    return true;
                });
            });
    };
    auto compact = [&](const compact_zset::view& packed, db::batch_entry&) {
        out.found = true;
        auto append = [&](uint64_t index) {
            const auto item = packed.at(index);
            out.values.push_back({item.first.str(), packed.score(item.second)});
            return true;
        };
        return sample_members(packed.count(), count, out.values, too_large,
            append, [&] {
                for (unsigned i = 0; i < packed.count(); ++i) append(i);
                return true;
            });
    };
    try {
        ok = run_callback(db, key, tree, false, compact);
    } catch (const std::length_error&) {
        too_large = true; ok = false;
    }
    if (too_large) read_status_ = read_status::sample_too_large;
    if (!ok) out = zset_sample_result();
    return ok;
}

} // namespace dao
} // namespace pkv
