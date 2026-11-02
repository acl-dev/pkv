#pragma once
#include <algorithm>
#include <iterator>
#include <random>
#include <set>
#include "zset_value.h"
#include <string>
#include <vector>

namespace pkv {

// Independent per worker; fibers do not yield while the engine is used. This
// provides non-cryptographic sampling, not tokens or authentication secrets.
std::mt19937_64& set_sampling_engine();

// Partial Fisher-Yates sampling is uniform without replacement; independent
// uniform indexes sample with replacement. Index pointers avoid copying every
// member just to return a small sample. Caller retains the set until completion.
template<class Range, class Generator, class Output, class Convert>
bool sample_collection(const Range& members, unsigned long long count, bool repeat,
        Generator& generator, std::vector<Output>& out, Convert convert) {
    out.clear();
    if (members.empty() || !count) return true;
    if (!repeat) count = std::min(count, static_cast<unsigned long long>(members.size()));
    if (count > out.max_size()) return false;
    out.reserve(static_cast<size_t>(count));
    if (!repeat && count == members.size()) {
        for (const auto& member : members) out.push_back(convert(member));
        return true;
    }
    if (count == 1) {
        auto member = members.begin();
        std::advance(member, std::uniform_int_distribution<size_t>(0, members.size() - 1)(generator));
        out.push_back(convert(*member));
        return true;
    }
    std::vector<const typename Range::value_type*> index;
    index.reserve(members.size());
    for (const auto& member : members) index.push_back(&member);
    for (size_t i = 0; i < static_cast<size_t>(count); ++i) {
        const auto position = std::uniform_int_distribution<size_t>(repeat ? 0 : i, index.size() - 1)(generator);
        if (!repeat) std::swap(index[i], index[position]);
        out.push_back(convert(*index[repeat ? position : i]));
    }
    return true;
}
template<class Generator>
bool sample_set(const std::set<std::string>& members, unsigned long long count, bool repeat,
        Generator& generator, std::vector<std::string>& out) {
    return sample_collection(members, count, repeat, generator, out, [](const std::string& value) { return value; });
}
template<class Generator>
bool sample_zset(const std::map<std::string, double>& members, unsigned long long count, bool repeat,
        Generator& generator, std::vector<zset_member>& out) {
    return sample_collection(members, count, repeat, generator, out,
        [](const std::pair<const std::string, double>& value) { return zset_member{value.first, value.second}; });
}

} // namespace pkv
