#pragma once
#include <algorithm>
#include <map>
#include <string>
#include <vector>

namespace pkv {
struct zset_member { std::string member; double score; };
struct zset_score { bool found = false; double score = 0; };
struct zset_rank { bool found = false; long long rank = 0; double score = 0; };
inline bool zset_less(const zset_member& a, const zset_member& b) {
    return a.score != b.score ? a.score < b.score : a.member < b.member;
}
inline std::vector<zset_member> ordered_zset(const std::map<std::string, double>& values) {
    std::vector<zset_member> out;
    out.reserve(values.size());
    for (const auto& value : values) out.push_back({value.first, value.second});
    std::sort(out.begin(), out.end(), zset_less);
    return out;
}
}
