#include "stdafx.h"
#include "paged_hash.h"
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
    rank_sampler ranks(size, repeat);
    for (uint64_t i = 0; i < wanted; ++i) {
        std::pair<std::string, std::string> field;
        bool found;
        if (!store.select(ranks.next(), field.first, found,
            with_values ? &field.second : nullptr)) return false;
        if (!found) {
            read_status_ = read_status::invalid_record; return false;
        }
        out.push_back(std::move(field));
    }
    return true;
}

bool paged_hash::sample(shared_db& db, const std::string& key, long long count,
    bool with_values, sample_values& out) {
    reset(); out.clear();
    if (count == LLONG_MIN) {
        read_status_ = read_status::invalid_integer; return false;
    }
    bool ok;
    try {
        ok = run(db, key, [&](map_store& store) {
            return choose(store, count, with_values, out);
        });
    } catch (const std::length_error&) {
        read_status_ = read_status::sample_too_large; ok = false;
    }
    if (!ok) out.clear();
    return ok;
}

} // namespace dao
} // namespace pkv
