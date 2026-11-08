#include "zset_store.h"
#include <algorithm>
#include <array>
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
    auto* scope = transaction_.scope_;
    if (scope && (entry_->action != db::update_action::keep ||
        entry_->clear_pages || transaction_.failed_ ||
        transaction_.has_changes())) scope = nullptr;
    const auto boundary = [&](double value, bool upper, uint64_t& rank,
        db::page_lock_scope::score_boundary* cached) {
        if (cached && cached->valid && cached->upper == upper &&
            cached->value == value) {
            rank = cached->rank;
            return true;
        }
        std::string bytes;
        if (!zset_page_codec::encode_score(value, bytes))
            return fail(status::invalid);
        if (!scores_->bound_rank(bytes, upper, rank, true)) return tree_error();
        if (cached) {
            cached->value = value;
            cached->rank = rank;
            cached->upper = upper;
            cached->valid = true;
        }
        return true;
    };
    if (!boundary(query.min_score.value, query.min_score.exclusive, first,
            scope ? &scope->lower_boundary_ : nullptr) ||
        !boundary(query.max_score.value, !query.max_score.exclusive, last,
            scope ? &scope->upper_boundary_ : nullptr)) return false;
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
        out.swap(values);
        return true;
    }
    uint64_t first, last;
    if (!bounds(query, first, last)) return false;
    if (first == last) return true;
    auto* scope = transaction_.scope_;
    if (scope && (last - first > db::page_lock_scope::max_cached_range ||
        entry_->action != db::update_action::keep || entry_->clear_pages ||
        transaction_.failed_ || transaction_.has_changes())) scope = nullptr;
    if (scope && scope->range_start_ == first && scope->range_end_ == last &&
        scope->range_reverse_ == query.reverse) {
        out = scope->range_members_;
        return true;
    }
    if (!collect(first, last, query.reverse, values)) return false;
    if (scope) {
        size_t remaining = 16384;
        bool fits = true;
        for (const auto& value : values) {
            if (value.member.size() > remaining) { fits = false; break; }
            remaining -= value.member.size();
        }
        if (fits) {
            scope->range_end_ = 0;
            try {
                scope->range_members_ = values;
                scope->range_start_ = first;
                scope->range_reverse_ = query.reverse;
                scope->range_end_ = last;
            } catch (const std::bad_alloc&) {
                // The result can still be returned without caching it.
            }
        }
    }
    out.swap(values);
    return true;
}

bool zset_store::collect(uint64_t first, uint64_t last, bool reverse,
    std::vector<zset_member>& out) {
    if (first == last) return true;
    const auto count = last - first;
    if (count >= 64 && count >= root_.elements - count) {
        if (!collect_all(out)) return false;
        out.resize(static_cast<size_t>(last));
        if (first) out.erase(out.begin(), out.begin() + first);
        if (reverse) std::reverse(out.begin(), out.end());
        return true;
    }
    constexpr size_t batch_capacity = 64;
    if (count >= 4) {
        struct validation {
            zset_store& store;
            std::vector<zset_member>& out;
            size_t start;
            size_t matched;
        } state{*this, out, 0, 0};
        auto member_found = [](void* raw, size_t index, const map_blob& value) {
            auto& job = *static_cast<validation*>(raw);
            auto& item = job.out[job.start + index];
            std::string bytes;
            if (value.head && !job.store.members_->read_blob(value, bytes))
                return job.store.tree_error();
            const auto& encoded = value.head ? bytes : value.inline_value;
            double score;
            if (!zset_page_codec::decode_value(encoded, score) || score != item.score)
                return job.store.fail(status::invalid);
            item.score = score;
            ++job.matched;
            return true;
        };
        std::array<map_store::lookup_request, batch_capacity> requests;
        auto validate = [&](size_t size) {
            for (size_t i = 0; i < size; ++i)
                requests[i] = {&out[state.start + i].member, i, 0};
            state.matched = 0;
            if (!members_->find_many(requests.data(), requests.data() + size,
                    member_found, &state)) return tree_error();
            if (state.matched != size) return fail(status::invalid);
            state.start += size;
            return true;
        };
        const bool visited = scores_->visit_score_range(first, last, reverse,
            [&](const std::string& key, const std::string&) {
                zset_member item;
                if (!zset_page_codec::decode_score_prefix(key, item.score))
                    return fail(status::invalid);
                item.member.assign(key, 8, std::string::npos);
                out.push_back(std::move(item));
                return out.size() - state.start != batch_capacity ||
                    validate(batch_capacity);
            });
        if (!visited) return tree_error();
        return validate(out.size() - state.start);
    }
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
