#include "stdafx.h"
#include "paged_zset.h"
#include "coder/compact_zset.h"
#include "common/rank_sampling.h"
#include <algorithm>
#include <stdexcept>

namespace pkv {
namespace dao {

namespace {

template<class Draw, class Visit>
bool sample_members(uint64_t size, long long count,
    std::vector<zset_member>& out, bool& too_large, Draw draw, Visit visit) {
    if (!size || !count) return true;
    const bool repeat = count < 0;
    const auto requested = static_cast<uint64_t>(repeat ? -count : count);
    const auto wanted = repeat ? requested : std::min(requested, size);
    if (wanted > out.max_size()) { too_large = true; return false; }
    out.reserve(static_cast<size_t>(wanted));
    if (!repeat && wanted == size) return visit();
    return draw(wanted, repeat);
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
        const auto size = store.metadata().elements;
        const auto select = [&](uint64_t rank) {
            zset_member value;
            bool present;
            if (!store.select(rank, value, present)) return false;
            if (!present) {
                read_status_ = read_status::invalid_record;
                return false;
            }
            out.values.push_back(std::move(value));
            return true;
        };
        return sample_members(size, count, out.values, too_large,
            [&](uint64_t wanted, bool repeat) {
                if (wanted < 4) return visit_sampled_ranks(size, wanted, repeat, select);
                std::array<uint64_t, zset_store::sample_rank_batch> ranks;
                size_t used = 0;
                bool fallback = false;
                const auto flush = [&] {
                    const auto offset = out.values.size();
                    out.values.resize(offset + used);
                    if (!store.select_many(ranks.data(), used, out.values.data() + offset,
                        fallback))
                        return false;
                    if (fallback) {
                        out.values.resize(offset);
                        for (size_t i = 0; i < used; ++i)
                            if (!select(ranks[i])) return false;
                    }
                    used = 0;
                    return true;
                };
                const bool ok = visit_sampled_ranks(size, wanted, repeat, [&](uint64_t rank) {
                    if (fallback) return select(rank);
                    ranks[used++] = rank;
                    return used < ranks.size() || flush();
                });
                return ok && (!used || flush());
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
            [&](uint64_t wanted, bool repeat) {
                return visit_sampled_ranks(packed.count(), wanted, repeat, append);
            }, [&] {
                for (unsigned i = 0; i < packed.count(); ++i) append(i);
                return true;
            });
    };
    try {
        ok = run_callback(db, key, std::ref(tree), false, std::ref(compact));
    } catch (const std::length_error&) {
        too_large = true; ok = false;
    }
    if (too_large) read_status_ = read_status::sample_too_large;
    if (!ok) out = zset_sample_result();
    return ok;
}

} // namespace dao
} // namespace pkv
