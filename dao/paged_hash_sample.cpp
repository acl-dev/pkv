#include "stdafx.h"
#include "paged_hash.h"
#include "coder/compact_hash.h"
#include "common/rank_sampling.h"
#include <stdexcept>

namespace pkv {
namespace dao {

bool paged_hash::choose(map_store& store, long long count, bool with_values,
    sample_values& out) {
    const auto size = store.metadata().elements;
    if (!size || !count) return true;
    const bool repeat = count < 0;
    auto wanted = static_cast<uint64_t>(repeat ? -count : count);
    if (!repeat) wanted = std::min(wanted, size);
    if (wanted > out.max_size()) {
        read_status_ = read_status::sample_too_large; return false;
    }
    out.reserve(static_cast<size_t>(wanted));
    if (!repeat && wanted == size) {
        if (!store.visit([&](const std::string& field,
            const std::string& value) {
            out.emplace_back(field, value); return true;
        }, with_values)) return false;
        if (out.size() != size) {
            read_status_ = read_status::invalid_record; return false;
        }
        return true;
    }
    return visit_sampled_ranks(size, wanted, repeat, [&](uint64_t rank) {
        std::pair<std::string, std::string> field;
        bool found;
        if (!store.select(rank, field.first, found,
            with_values ? &field.second : nullptr)) return false;
        if (!found) {
            read_status_ = read_status::invalid_record; return false;
        }
        out.push_back(std::move(field));
        return true;
    });
}

bool paged_hash::sample(shared_db& db, const std::string& key, long long count,
    bool with_values, sample_values& out) {
    reset(); out.clear();
    if (count == LLONG_MIN) {
        read_status_ = read_status::invalid_integer; return false;
    }
    bool ok = false;
    bool too_large = false;
    try {
        const auto tree = [&](map_store& store) {
            return choose(store, count, with_values, out);
        };
        ok = run_packed(db, key, tree,
            [&](db::batch_entry&, const compact_hash::view* packed) {
                if (!packed || !count) return true;
                const auto size = packed->count();
                const bool repeat = count < 0;
                const auto wanted = repeat ? static_cast<uint64_t>(-count) :
                    std::min<uint64_t>(count, size);
                if (wanted > out.max_size()) {
                    too_large = true;
                    return true;
                }
                out.reserve(static_cast<size_t>(wanted));
                const auto append = [&](unsigned rank) {
                    const auto item = packed->at(rank);
                    out.emplace_back(item.first.str(),
                        with_values ? item.second.str() : std::string());
                };
                if (!repeat && wanted == size) {
                    for (unsigned i = 0; i < size; ++i) append(i);
                } else if (repeat || wanted == 1) {
                    rank_sampler ranks(size, true);
                    for (uint64_t i = 0; i < wanted; ++i) append(ranks.next());
                } else {
                    std::array<unsigned, compact_hash::max_fields> ranks;
                    std::iota(ranks.begin(), ranks.begin() + size, 0U);
                    auto& random = set_sampling_engine();
                    for (unsigned i = 0; i < wanted; ++i) {
                        const auto position =
                            std::uniform_int_distribution<unsigned>(i, size - 1)(random);
                        std::swap(ranks[i], ranks[position]);
                        append(ranks[i]);
                    }
                }
                return true;
            });
    } catch (const std::length_error&) {
        too_large = true;
    }
    if (too_large) {
        read_status_ = read_status::sample_too_large;
        ok = false;
    }
    if (!ok) out.clear();
    return ok;
}

} // namespace dao
} // namespace pkv
