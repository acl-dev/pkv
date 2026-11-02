#include "zset_store.h"
#include <algorithm>
#include <cmath>

namespace pkv {
namespace {
void slice(const zset_query& query, uint64_t& first, uint64_t& last) {
    const auto skip = std::min(last - first,
        static_cast<uint64_t>(query.offset));
    if (query.reverse) last -= skip;
    else first += skip;
    if (query.limit > 0) {
        const auto count = std::min(last - first,
            static_cast<uint64_t>(query.limit));
        if (query.reverse) first = last - count;
        else last = first + count;
    }
}
}

bool zset_store::bounds(const zset_query& query, uint64_t& first,
    uint64_t& last) {
    first = last = 0;
    if (error_ != status::ok || !scores_) return false;
    if (query.mode == zrange_mode::rank) {
        size_t offset, count;
        if (!list_slice(static_cast<size_t>(root_.elements), query.start,
            query.end, offset, count)) return true;
        first = query.reverse ? root_.elements - offset - count : offset;
        last = first + count;
        return true;
    }
    if (query.mode != zrange_mode::score) return fail(status::invalid);
    if (std::isnan(query.min_score.value) || std::isnan(query.max_score.value))
        return fail(status::invalid);
    if (zscore_empty(query) || query.offset < 0 || query.limit == 0)
        return true;
    std::string lower, upper;
    if (!zset_page_codec::encode_score(query.min_score.value, lower) ||
        !zset_page_codec::encode_score(query.max_score.value, upper))
        return fail(status::invalid);
    if (!scores_->bound_rank(lower, query.min_score.exclusive, first, true)
        || !scores_->bound_rank(upper, !query.max_score.exclusive, last, true))
        return tree_error();
    if (last < first) return fail(status::invalid);
    slice(query, first, last);
    return true;
}

bool zset_store::visit_range(const zset_query& query,
    const visitor& callback) {
    if (query.mode == zrange_mode::lex) return fail(status::invalid);
    uint64_t first, last;
    if (!bounds(query, first, last)) return false;
    if (first == last) return true;
    bool valid = true;
    if (!scores_->visit_rank(query.reverse ? last - 1 : first, last - first,
        query.reverse, [&](const std::string& key, const std::string&) {
        zset_member value;
        if (!ordered_member(key, value)) { valid = false; return false; }
        return callback(value.member, value.score);
    }, false)) return tree_error();
    return valid;
}

bool zset_store::range(const zset_query& query,
    std::vector<zset_member>& out) {
    out.clear();
    if (error_ != status::ok || !scores_) return false;
    std::vector<zset_member> values;
    if (query.mode == zrange_mode::lex) {
        long long ignored;
        if (!lex_range(query, &values, ignored)) return false;
    } else {
        uint64_t first, last;
        if (!bounds(query, first, last)) return false;
        if (first == last) return true;
        if (!collect(first, last, query.reverse, values)) return false;
    }
    out.swap(values);
    return true;
}

bool zset_store::collect(uint64_t first, uint64_t last, bool reverse,
    std::vector<zset_member>& out) {
    if (first == last) return true;
    bool valid = true;
    const bool visited = scores_->visit_rank(reverse ? last - 1 : first,
        last - first, reverse,
        [&](const std::string& key, const std::string&) {
        zset_member value;
        if (!ordered_member(key, value)) { valid = false; return false; }
        out.push_back(std::move(value));
        return true;
    }, false);
    if (!visited) return tree_error();
    return valid;
}

bool zset_store::lex_bounds(const zset_query& query, uint64_t& first,
    uint64_t& last, bool& indexed) {
    first = last = 0; indexed = true;
    if (!root_.elements) return true;
    map_node node;
    if (!members_->load(members_->root_.tree, node) || !members_->check(node))
        return tree_error();
    double score;
    std::string prefix;
    if (node.value.head || !zset_page_codec::decode_value(
        node.value.inline_value, score) ||
        !zset_page_codec::encode_score(score, prefix))
        return fail(status::invalid);
    if (!scores_->bound_rank(prefix, false, first, true) ||
        !scores_->bound_rank(prefix, true, last, true)) return tree_error();
    if (first || last != root_.elements) { indexed = false; return true; }
    if (query.min_lex.infinity != -1 && !scores_->bound_rank(
        prefix + query.min_lex.value, query.min_lex.exclusive, first))
        return tree_error();
    if (query.max_lex.infinity != 1 && !scores_->bound_rank(
        prefix + query.max_lex.value, !query.max_lex.exclusive, last))
        return tree_error();
    if (last < first) return fail(status::invalid);
    slice(query, first, last);
    return true;
}

bool zset_store::count(const zset_query& query, long long& out) {
    out = 0;
    if (error_ != status::ok || !scores_) return false;
    if (query.mode == zrange_mode::lex) return lex_range(query, nullptr, out);
    uint64_t first, last;
    if (!bounds(query, first, last)) return false;
    out = static_cast<long long>(last - first);
    return true;
}

bool zset_store::lex_range(const zset_query& query,
    std::vector<zset_member>* out, long long& count) {
    count = 0;
    if (zlex_empty(query) || query.offset < 0 || query.limit == 0 ||
        static_cast<uint64_t>(query.offset) >= root_.elements) return true;
    uint64_t first, last;
    bool indexed;
    if (!lex_bounds(query, first, last, indexed)) return false;
    if (indexed) {
        if (out && !collect(first, last, query.reverse, *out)) return false;
        count = static_cast<long long>(last - first);
        return true;
    }
    // Member lookup uses scan buckets. Mixed-score lex queries retain the
    // legacy DAO's member-byte semantics using a bounded top-k response map.
    // Unlimited responses necessarily retain the requested output strings.
    const uint64_t wanted = query.limit < 0 ? root_.elements :
        std::min(root_.elements, static_cast<uint64_t>(query.offset) +
            static_cast<uint64_t>(query.limit));
    std::map<std::string, double> selected;
    uint64_t matched = 0;
    if (!visit([&](const std::string& member, double score) {
        if (!zlex_matches(member, query)) return true;
        ++matched;
        if (!out) return true;
        selected.emplace(member, score);
        if (selected.size() > wanted)
            selected.erase(query.reverse ? selected.begin() :
                std::prev(selected.end()));
        return true;
    })) return false;
    const auto skip = std::min(matched, static_cast<uint64_t>(query.offset));
    uint64_t amount = matched - skip;
    if (query.limit > 0)
        amount = std::min(amount, static_cast<uint64_t>(query.limit));
    if (out && amount) {
        uint64_t ignored = 0;
        const auto append = [&](const std::string& member, double score) {
            if (ignored++ >= skip) out->push_back({member, score});
        };
        if (query.reverse) {
            for (auto it = selected.rbegin(); it != selected.rend(); ++it)
                append(it->first, it->second);
        } else {
            for (const auto& item : selected) append(item.first, item.second);
        }
        if (out->size() != amount) return fail(status::invalid);
    }
    count = static_cast<long long>(amount);
    return true;
}

} // namespace pkv
