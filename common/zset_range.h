#pragma once
#include "zset_value.h"
#include "redis_list_util.h"

namespace pkv {
enum class zrange_mode { rank, score, lex };
struct zscore_bound { double value = 0; bool exclusive = false; };
struct zlex_bound { int infinity = 0; std::string value; bool exclusive = false; };
struct zset_query {
    zrange_mode mode = zrange_mode::rank;
    bool reverse = false;
    long long start = 0, end = -1;
    zscore_bound min_score, max_score;
    zlex_bound min_lex, max_lex;
    long long offset = 0, limit = -1;
};
bool parse_zscore_bound(const std::string& text, zscore_bound& out);
bool parse_zlex_bound(const std::string& text, zlex_bound& out);
inline bool zscore_empty(const zset_query& query) {
    return query.min_score.value > query.max_score.value ||
        (query.min_score.value == query.max_score.value && (query.min_score.exclusive || query.max_score.exclusive));
}
inline bool zlex_empty(const zset_query& query) {
    if (query.min_lex.infinity == 1 || query.max_lex.infinity == -1) return true;
    if (query.min_lex.infinity == -1 || query.max_lex.infinity == 1) return false;
    return query.min_lex.value > query.max_lex.value ||
        (query.min_lex.value == query.max_lex.value && (query.min_lex.exclusive || query.max_lex.exclusive));
}
inline bool zscore_matches(double score, const zset_query& query) {
    return (query.min_score.exclusive ? score > query.min_score.value : score >= query.min_score.value) &&
        (query.max_score.exclusive ? score < query.max_score.value : score <= query.max_score.value);
}
inline bool zlex_matches(const std::string& value, const zset_query& query) {
    return !zlex_empty(query) &&
        (query.min_lex.infinity == -1 || (query.min_lex.exclusive ? value > query.min_lex.value : value >= query.min_lex.value)) &&
        (query.max_lex.infinity == 1 || (query.max_lex.exclusive ? value < query.max_lex.value : value <= query.max_lex.value));
}
inline void select_zset(const std::map<std::string, double>& values, const zset_query& query, std::vector<zset_member>& out) {
    out.clear();
    std::vector<zset_member> ordered;
    if (query.mode == zrange_mode::lex) {
        for (const auto& value : values) ordered.push_back({value.first, value.second});
    } else ordered = ordered_zset(values);
    if (query.reverse) std::reverse(ordered.begin(), ordered.end());
    if (query.mode == zrange_mode::rank) {
        size_t offset, count;
        if (list_slice(ordered.size(), query.start, query.end, offset, count))
            out.assign(ordered.begin() + offset, ordered.begin() + offset + count);
        return;
    }
    if (query.offset < 0 || query.limit == 0) return;
    auto skip = query.offset;
    for (const auto& value : ordered) {
        if (!(query.mode == zrange_mode::score ? zscore_matches(value.score, query) : zlex_matches(value.member, query))) continue;
        if (skip) { --skip; continue; }
        out.push_back(value);
        if (query.limit > 0 && out.size() >= static_cast<unsigned long long>(query.limit)) break;
    }
}
}
