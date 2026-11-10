#include "zset_store.h"
#include "coder/compact_zset.h"
#include <unordered_map>
#include "common/redis_double.h"
#include <algorithm>

namespace pkv {

bool zset_store::rank(const std::string& member, zset_rank& out) {
    out = zset_rank();
    if (error_ != status::ok || !members_ || !scores_) return false;
    auto* scope = repeated_read(member);
    if (scope && scope->member_rank_ >= 0 && scope->scored_member_ &&
        *scope->scored_member_ == member) {
        out.found = true;
        out.rank = scope->member_rank_;
        out.score = scope->member_score_;
        return true;
    }
    bool found;
    double score;
    if (!read_member_score(member, score, found)) return false;
    if (!found) return true;
    detail::node_key_workspace key_buffer(reuse_score_key(member));
    auto& key = key_buffer.key.inline_value;
    if (!zset_page_codec::score_key(score, member, key))
        return fail(status::invalid);
    uint64_t rank;
    if (!scores_->rank_in_tree(key, rank, found, true)) return tree_error();
    if (!found || rank >= root_.elements) return fail(status::invalid);
    out.found = true; out.rank = static_cast<long long>(rank);
    out.score = score;
    if (scope) {
        scope->scored_member_ = &member;
        scope->member_rank_ = out.rank;
        scope->member_score_ = out.score;
    }
    return true;
}

bool zset_store::ordered_member(const std::string& key, zset_member& out,
    bool checked_score_key) {
    zset_member value;
    if (!zset_page_codec::decode_score_prefix(key, value.score))
        return fail(status::invalid);
    value.member.assign(key, 8, std::string::npos);
    bool found;
    double score;
    if (!read_member_score(value.member, score, found)) return false;
    if (!found || score != value.score)
        return fail(status::invalid);
    if (!checked_score_key) {
        // The score prefix and member already match; verify the same key is
        // reachable without rebuilding its byte representation.
        uint64_t length;
        if (!scores_->length(key, length, found)) return tree_error();
        if (!found || length) return fail(status::invalid);
    }
    value.score = score;
    out = std::move(value);
    return true;
}

bool zset_store::select(uint64_t rank, zset_member& out, bool& found) {
    found = false;
    if (error_ != status::ok || !scores_) return false;
    std::string key;
    bool present;
    if (!scores_->select_score_key(rank, key, present)) return tree_error();
    if (!present) return true;
    if (!ordered_member(key, out, true)) return false;
    found = true;
    return true;
}

bool zset_store::select_many(const uint64_t* ranks, size_t count, zset_member* out,
    bool& fallback) {
    fallback = false;
    if (error_ != status::ok || !members_ || !scores_) return false;
    if (!count || count > sample_rank_batch) return fail(status::invalid);
    std::array<map_store::score_rank_request, sample_rank_batch> requests;
    for (size_t i = 0; i < count; ++i) requests[i] = {ranks[i], i};
    if (!scores_->select_score_members(requests.data(), requests.data() + count,
        out, fallback)) return fallback || tree_error();
    return validate_score_members(out, count);
}

bool zset_store::visit(const visitor& callback) {
    if (error_ != status::ok || !members_) return false;
    bool valid = true;
    const bool visited = members_->visit([&](const std::string& member,
        const std::string& bytes) {
        double score;
        if (!zset_page_codec::decode_value(bytes, score)) {
            valid = false; return fail(status::invalid);
        }
        zset_score checked;
        if (!get(member, checked)) { valid = false; return false; }
        return callback(member, score);
    });
    if (!visited) return tree_error();
    return valid;
}

bool zset_store::visit_batches(size_t batch_size, const batch_visitor& callback) {
    if (error_ != status::ok || !members_) return false;
    if (!batch_size) return true;
    std::array<std::string, visit_batch_capacity> members;
    std::array<const std::string*, visit_batch_capacity> keys;
    std::array<double, visit_batch_capacity> expected;
    std::array<zset_score, visit_batch_capacity> checked;
    size_t used = 0;
    const auto flush = [&]() {
        if (!used) return true;
        if (used == 1) {
            if (!get(*keys[0], checked[0])) return false;
        } else if (!get_scores_batched(keys.data(), used, checked.data())) return false;
        for (size_t i = 0; i < used; ++i) {
            if (!checked[i].found || checked[i].score != expected[i])
                return fail(status::invalid);
        }
        batch_size = callback(keys.data(), used);
        used = 0;
        return true;
    };
    bool valid = true;
    const bool visited = members_->visit([&](const std::string& member,
        const std::string& bytes) {
        double score;
        if (!zset_page_codec::decode_value(bytes, score)) {
            valid = false;
            return fail(status::invalid);
        }
        const bool large = member.size() > map_page_codec::zset_key_inline_bytes;
        if (large) {
            valid = flush();
            if (!valid || !batch_size) return false;
            keys[used] = &member;
        } else {
            members[used] = member;
            keys[used] = &members[used];
        }
        expected[used++] = score;
        if (large || used == members.size() || used == batch_size) valid = flush();
        return valid && batch_size;
    });
    if (!visited) return tree_error();
    return valid && flush();
}

bool zset_store::visit_ordered(const visitor& callback) {
    if (error_ != status::ok || !scores_) return false;
    bool valid = true;
    const bool visited = scores_->visit([&](const std::string& key,
        const std::string&) {
        zset_member value;
        if (!ordered_member(key, value)) { valid = false; return false; }
        return callback(value.member, value.score);
    }, false);
    if (!visited) return tree_error();
    return valid;
}

bool zset_store::collect_all(std::vector<zset_member>& values) {
    unsigned previous_bucket = 0;
    if (!members_->visit_all([&](std::string&& member, std::string&& bytes) {
        const auto bucket = members_->bucket(member);
        if (!values.empty() && (bucket < previous_bucket ||
            (bucket == previous_bucket && values.back().member >= member)))
            return fail(status::invalid);
        double score;
        if (!zset_page_codec::decode_value(bytes, score)) return fail(status::invalid);
        previous_bucket = bucket;
        values.push_back({std::move(member), score});
        return true;
    }, true)) return tree_error();
    if (values.size() != root_.elements) return fail(status::invalid);

    // Match the two indexes in score order without looking up each member
    // again. The owning walk validates each subtree summary once.
    std::sort(values.begin(), values.end(), [](const zset_member& a, const zset_member& b) {
        return a.score == b.score ? a.member < b.member : a.score < b.score;
    });
    size_t matched = 0;
    if (!scores_->visit_all([&](std::string&& key, std::string&& bytes) {
        double score;
        if (matched == values.size() || !bytes.empty() ||
            !zset_page_codec::decode_score_prefix(key, score))
            return fail(status::invalid);
        const auto& value = values[matched++];
        return (score == value.score &&
            key.compare(8, std::string::npos, value.member) == 0) ||
            fail(status::invalid);
    }, true)) return tree_error();
    return matched == values.size() || fail(status::invalid);
}

bool zset_store::pop_rebuild(uint64_t count, bool maximum,
    std::vector<zset_member>& out, bool compact_remainder) {
    if (error_ != status::ok || !scores_) return false;
    if (count > root_.elements) return fail(status::invalid);
    std::vector<zset_member> values;
    if (!collect_all(values)) return false;
    if (maximum) std::reverse(values.begin(), values.end());
    if (count == values.size()) {
        if (!values.empty() && !clear()) return false;
    } else {
        const auto remaining = values.size() - static_cast<size_t>(count);
        std::string packed;
        if (compact_remainder && remaining <= compact_zset::max_members &&
            transaction_.database_.supports_compact_zsets()) {
            std::array<compact_zset::entry_view, compact_zset::max_members> entries;
            for (size_t i = 0; i < remaining; ++i) {
                const auto& item = values[static_cast<size_t>(count) + i];
                entries[i] = {item.member, item.score};
            }
            packed = compact_zset::try_encode(entries.data(), remaining,
                root_.expires_at_ms);
        }
        if (packed.empty()) {
            if (!rebuild_remainder(values, static_cast<size_t>(count), maximum,
                root_.expires_at_ms)) return false;
        } else {
            // Keep clear_pages set while replacing the paged root. Finish
            // the empty embedded stores without encoding an empty ZSet.
            if (!clear()) return false;
            if (!members_->finish() || !scores_->finish()) return tree_error();
            entry_->value = std::move(packed);
            entry_->maintenance_only = false;
            dirty_ = false;
        }
        values.resize(static_cast<size_t>(count));
    }
    out.swap(values);
    return true;
}

bool zset_store::build(std::vector<zset_member>& values, int64_t expires_at_ms) {
    std::sort(values.begin(), values.end(), zset_less);
    return rebuild_remainder(values, 0, false, expires_at_ms);
}

bool zset_store::rebuild_remainder(const std::vector<zset_member>& values,
    size_t first, bool descending, int64_t expires_at_ms) {
    return build_indexes(values.size() - first, expires_at_ms, [&](size_t i) {
        const auto& value = values[descending ? values.size() - i - 1 : first + i];
        return member_ref{&value.member, value.score};
    });
}

bool zset_store::build_borrowed(std::vector<member_ref>& values,
    int64_t expires_at_ms) {
    std::sort(values.begin(), values.end(), [](const member_ref& a, const member_ref& b) {
        return a.score != b.score ? a.score < b.score : *a.member < *b.member;
    });
    return build_indexes(values.size(), expires_at_ms,
        [&](size_t i) { return values[i]; });
}

template<class MemberAt>
bool zset_store::build_indexes(size_t remaining, int64_t expires_at_ms,
    const MemberAt& at) {
    initialize(expires_at_ms);
    if (error_ != status::ok) return false;
    // Match ordinary insertion for keys just beyond the legacy inline limit.
    const bool compact = transaction_.database_.supports_compact_zsets();
    const auto key_limit = compact ?
        map_page_codec::zset_key_inline_bytes : map_page_codec::key_inline_bytes;
    uint32_t height;
    std::unordered_map<const std::string*, uint64_t> member_fragments;
    {
        struct bucketed_member { member_ref value; unsigned bucket; };
        std::vector<bucketed_member> ordered;
        ordered.reserve(remaining);
        for (size_t i = 0; i < remaining; ++i) {
            const auto value = at(i);
            if (value.member->size() > map_page_codec::max_payload_bytes)
                return fail(status::too_large);
            if (compact && !root_.crc_member_buckets &&
                value.member->size() >= map_store::crc_bucket_min)
                use_crc_member_buckets(true);
            ordered.push_back({value, members_->bucket(*value.member)});
        }
        const auto by_key = [](const bucketed_member& a,
            const bucketed_member& b) { return *a.value.member < *b.value.member; };
        if (remaining >= 1024) {
            map_store::sort_member_buckets(ordered, by_key);
        } else std::sort(ordered.begin(), ordered.end(), [&](const bucketed_member& a,
            const bucketed_member& b) {
            if (a.bucket != b.bucket) return a.bucket < b.bucket;
            return by_key(a, b);
        });
        transaction_.reserve_sorted_pages(entry_, remaining * 2);
        map_node member_node;
        auto fill_member = [&](size_t i, map_node& current) {
            const auto& member = ordered[i];
            current.bucket = static_cast<uint8_t>(member.bucket);
            if (!zset_page_codec::encode_value(member.value.score, current.value.inline_value))
                return fail(status::invalid);
            current.value.length = current.value.inline_value.size();
            if (!members_->store_bulk_blob(*member.value.member, key_limit, current.key))
                return false;
            if (compact && current.key.head)
                member_fragments.emplace(member.value.member, current.key.head);
            return true;
        };
        if (!members_->build_tree_from(0, remaining, members_->root_.tree, height,
            member_node, fill_member))
            return tree_error();
    }

    // Rank order is already known. Encode each score key when its node is
    // visited, keeping only one scratch string for fragmented keys.
    map_node node;
    std::string fragment_key;
    auto fill_score = [&](size_t i, map_node& current) {
        const auto value = at(i);
        if (compact && value.member->size() > key_limit) {
            const auto found = member_fragments.find(value.member);
            if (found == member_fragments.end() ||
                !zset_page_codec::score_key(value.score, "", current.key.inline_value))
                return fail(status::invalid);
            current.key.length = value.member->size() + 8;
            current.key.head = found->second;
            return true;
        }
        if (value.member->size() > key_limit - 8) {
            if (!zset_page_codec::score_key(value.score, *value.member, fragment_key))
                return fail(status::invalid);
            return scores_->store_blob(fragment_key, key_limit, current.key);
        }
        if (!zset_page_codec::score_key(value.score, *value.member, current.key.inline_value))
            return fail(status::invalid);
        current.key.head = 0;
        current.key.length = current.key.inline_value.size();
        return true;
    };
    if (!scores_->build_tree_from(0, remaining, scores_->root_.tree, height, node, fill_score))
        return tree_error();
    members_->root_.elements = scores_->root_.elements = remaining;
    return counts();
}

bool zset_store::scan(const scan_options& options, scan_page& out) {
    out = scan_page();
    if (error_ != status::ok || !members_) return false;
    scan_options request = options;
    request.no_values = false;
    scan_page page;
    if (!members_->scan(request, page)) return tree_error();
    if (page.values.size() % 2) return fail(status::invalid);
    for (size_t i = 1; i < page.values.size(); i += 2) {
        double score;
        if (!zset_page_codec::decode_value(page.values[i], score))
            return fail(status::invalid);
        zset_score checked;
        if (!get(page.values[i - 1], checked)) return false;
        page.values[i] = format_redis_double(score);
    }
    out = std::move(page);
    return true;
}

} // namespace pkv
