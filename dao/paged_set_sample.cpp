#include "stdafx.h"
#include "paged_set.h"
#include "coder/compact_set.h"
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
        auto collect = [&](uint64_t, const std::string& member) {
            out.push_back(member);
        };
        return visit_members(store, std::ref(collect));
    }
    return visit_sampled_ranks(size, wanted, repeat, [&](uint64_t rank) {
        std::string member;
        bool present;
        if (!store.select(rank, member, present)) return false;
        if (!present) {
            read_status_ = read_status::invalid_record; return false;
        }
        out.push_back(std::move(member));
        return true;
    });
}

bool paged_set::sample(shared_db& db, const std::string& key,
    long long count, bool remove, set_sample_result& out) {
    reset(); out = set_sample_result();
    if (count == LLONG_MIN || (remove && count < 0)) {
        read_status_ = read_status::invalid_integer; return false;
    }
    bool ok = false;
    bool too_large = false;
    try {
        auto tree = [&](map_store& store) {
            out.found = store.exists();
            const auto size = store.metadata().elements;
            const auto wanted = static_cast<uint64_t>(count);
            if (remove && out.found && wanted && (wanted == 1 || wanted < size)) {
                if (wanted > out.values.max_size()) {
                    read_status_ = read_status::sample_too_large;
                    return false;
                }
                out.values.resize(static_cast<size_t>(wanted));
                return store.take_set_sample(out.values);
            }
            if (!choose(store, count, out.values)) return false;
            if (remove && out.found && wanted >= size) {
                for (const auto& member : out.values) {
                    // Full traversal already checked inline members. Erase
                    // fragmented keys individually to detect shared chains.
                    if (member.size() <= map_page_codec::key_inline_bytes)
                        continue;
                    bool removed;
                    if (!store.erase(member, removed)) return false;
                    if (!removed) {
                        read_status_ = read_status::invalid_record;
                        return false;
                    }
                }
                if (store.exists()) return store.clear();
            }
            return true;
        };
        const auto compact = [&](db::batch_entry& root, const compact_set::view* packed) {
            out.found = packed != nullptr;
            if (!packed || !count) return true;

            const auto size = packed->count();
            const bool repeat = count < 0;
            const auto wanted = repeat ? static_cast<uint64_t>(-count) :
                std::min<uint64_t>(count, size);
            if (wanted > out.values.max_size()) {
                too_large = true;
                return true;
            }
            out.values.reserve(static_cast<size_t>(wanted));
            compact_set::erase_mask removed;
            if (!repeat && wanted == size) {
                for (unsigned i = 0; i < size; ++i)
                    out.values.push_back(packed->at(i).str());
            } else if (repeat || wanted == 1) {
                rank_sampler ranks(size, true);
                for (uint64_t i = 0; i < wanted; ++i) {
                    const auto rank = ranks.next();
                    out.values.push_back(packed->at(rank).str());
                    if (remove) removed.set(static_cast<size_t>(rank));
                }
            } else {
                std::array<unsigned, compact_set::max_members> ranks;
                std::iota(ranks.begin(), ranks.begin() + size, 0U);
                auto& random = set_sampling_engine();
                for (unsigned i = 0; i < wanted; ++i) {
                    const auto position =
                        std::uniform_int_distribution<unsigned>(i, size - 1)(random);
                    std::swap(ranks[i], ranks[position]);
                    out.values.push_back(packed->at(ranks[i]).str());
                    if (remove) removed.set(ranks[i]);
                }
            }
            if (remove && wanted == size) {
                root.action = db::update_action::erase;
            } else if (remove) {
                root.value = packed->without(removed, size - wanted);
                root.action = db::update_action::write;
            }
            return true;
        };
        ok = run_callback(db, key, std::ref(tree), true, std::ref(compact));
    } catch (const std::length_error&) {
        too_large = true;
    }
    if (too_large) {
        read_status_ = read_status::sample_too_large;
        ok = false;
    }
    if (!ok) out = set_sample_result();
    return ok;
}

} // namespace dao
} // namespace pkv
