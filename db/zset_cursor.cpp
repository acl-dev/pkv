#include "zset_store.h"
#include "common/redis_double.h"

namespace pkv {

bool zset_store::rank(const std::string& member, zset_rank& out) {
    out = zset_rank();
    zset_score score;
    if (!get(member, score)) return false;
    if (!score.found) return true;
    std::string key;
    if (!zset_page_codec::score_key(score.score, member, key))
        return fail(status::invalid);
    uint64_t rank;
    bool found;
    if (!scores_->rank(key, rank, found)) return tree_error();
    if (!found || rank >= root_.elements) return fail(status::invalid);
    out.found = true; out.rank = static_cast<long long>(rank);
    out.score = score.score;
    return true;
}

bool zset_store::ordered_member(const std::string& key, zset_member& out) {
    if (key.size() < 8) return fail(status::invalid);
    zset_member value;
    if (!zset_page_codec::decode_score(key.substr(0, 8), value.score))
        return fail(status::invalid);
    value.member.assign(key, 8, std::string::npos);
    zset_score score;
    if (!get(value.member, score)) return false;
    if (!score.found || score.score != value.score)
        return fail(status::invalid);
    value.score = score.score;
    out = std::move(value);
    return true;
}

bool zset_store::select(uint64_t rank, zset_member& out, bool& found) {
    found = false;
    if (error_ != status::ok || !scores_) return false;
    std::string key;
    bool present;
    if (!scores_->select(rank, key, present)) return tree_error();
    if (!present) return true;
    if (!ordered_member(key, out)) return false;
    found = true;
    return true;
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
