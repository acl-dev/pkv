#include "packed_hash_store.h"
#include "map_store.h"
#include "coder/zset_page.h"
#include "common/zset_value.h"
#include <algorithm>
#include <cstring>

namespace pkv {

bool map_store::prefix_order(const map_blob& key, const std::string& input,
    int& order) {
    order = 0;
    if (!key.head) {
        const auto& bytes = key.inline_value;
        const auto length = std::min(bytes.size(), input.size());
        order = std::memcmp(bytes.data(), input.data(), length);
        if (!order && bytes.size() < input.size()) order = -1;
        return true;
    }
    blob_reader reader(*this, key);
    size_t offset = 0;
    while (offset < input.size()) {
        map_fragment_view page;
        bool present;
        if (!reader.next(page, present)) return false;
        if (!present) { order = -1; return true; }
        const auto length = std::min(page.size, input.size() - offset);
        order = std::memcmp(page.data, input.data() + offset, length);
        if (order) return true;
        offset += length;
    }
    return true;
}

bool map_store::bound_rank(const std::string& key, bool upper, uint64_t& rank,
    bool prefix) {
    rank = 0;
    if (error_ != status::ok) return false;
    if (prefix && !lexical_) return fail(status::invalid);
    if (root_.bucketed) return bucket_bound(key, upper, rank);
    if ((root_.tree == 0) != (root_.elements == 0)) return fail(status::invalid);
    uint64_t id = root_.tree, before = 0, elements = root_.elements + 1;
    uint32_t height = map_page_codec::max_height + 1;
    const auto key_bucket = bucket(key);
    const bool reuse = reuse_node_keys(key);
    node_children children(reuse);
    detail::node_key_workspace node(reuse);
    while (id) {
        if (!load_child(id, node, children) || !check(node, &children))
            return false;
        if (node.height >= height || node.elements >= elements ||
            (id == root_.tree && node.elements != root_.elements))
            return fail(status::invalid);
        const uint64_t left_count = node.left ? children.left.elements : 0;
        int comparison;
        if (!(prefix ? prefix_order(node.key, key, comparison) :
            order(node, key, key_bucket, comparison))) return false;
        height = node.height; elements = node.elements;
        if (comparison < 0 || (upper && comparison == 0)) {
            before += left_count + 1; id = node.right;
        } else id = node.left;
    }
    rank = before;
    return true;
}

map_store::rank_cursor::rank_cursor(map_store& store, bool reverse)
: store_(store), reverse_(reverse) {}

bool map_store::rank_cursor::seek(uint64_t rank) {
    if (!store_.root_.tree || rank >= store_.root_.elements)
        return store_.fail(status::invalid);
    path_.clear();
    uint64_t id = store_.root_.tree, elements = store_.root_.elements + 1;
    uint32_t height = map_page_codec::max_height + 1;
    node_children children(false);
    while (id) {
        map_node node;
        if (!store_.load_child(id, node, children) ||
            !store_.check(node, &children)) return false;
        if (node.height >= height || node.elements >= elements ||
            (id == store_.root_.tree && node.elements != store_.root_.elements))
            return store_.fail(status::invalid);
        const uint64_t left_count = node.left ? children.left.elements : 0;
        height = node.height; elements = node.elements;
        if (rank == left_count) {
            path_.push_back(std::move(node)); return true;
        }
        if (rank < left_count) {
            id = node.left;
            if (!reverse_) path_.push_back(std::move(node));
        } else {
            rank -= left_count + 1; id = node.right;
            if (reverse_) path_.push_back(std::move(node));
        }
    }
    return store_.fail(status::invalid);
}

bool map_store::rank_cursor::descend(uint64_t id, uint32_t height,
    uint64_t elements) {
    node_children children(false);
    while (id) {
        map_node node;
        node_summary left, right;
        if (!store_.load_child(id, node, children) ||
            !store_.check_path(node, !reverse_, children, left, right))
            return false;
        if (node.height >= height || node.elements >= elements ||
            path_.size() >= map_page_codec::max_height)
            return store_.fail(status::invalid);
        height = node.height; elements = node.elements;
        id = reverse_ ? node.right : node.left;
        path_.push_back(std::move(node));
    }
    return true;
}

bool map_store::rank_cursor::next(map_node& node) {
    if (path_.empty()) return store_.fail(status::invalid);
    map_node value = std::move(path_.back()); path_.pop_back();
    if (!descend(reverse_ ? value.left : value.right, value.height,
        value.elements)) return false;
    node = std::move(value);
    return true;
}

bool map_store::visit_score_range(uint64_t first, uint64_t last, bool reverse,
    const visitor& callback) {
    if (!embedded_ || !lexical_ || first >= last || last > root_.elements)
        return fail(status::invalid);
    map_node root;
    if (!load(root_.tree, root)) return false;
    if (root.elements != root_.elements) return fail(status::invalid);
    return visit_score_path(root, first, last, reverse, nullptr, nullptr, callback);
}

bool map_store::select_score_members(score_rank_request* first,
    score_rank_request* last, zset_member* members, bool& fragmented) {
    if (!embedded_ || !lexical_ || first == last) return fail(status::invalid);
    std::sort(first, last, [](const score_rank_request& a, const score_rank_request& b) {
        return a.rank < b.rank;
    });
    if ((last - 1)->rank >= root_.elements) return fail(status::invalid);
    score_rank_node root;
    if (!root_.tree) return fail(status::invalid);
    if (!load_score_rank_node(root_.tree, 0, root)) return false;
    if (root.summary.elements != root_.elements) return fail(status::invalid);
    return select_score_paths(root, 0, first, last, nullptr, nullptr, members, fragmented);
}

bool map_store::select_score_paths(const score_rank_node& node, uint64_t base,
    const score_rank_request* first, const score_rank_request* last,
    const score_rank_node* lower, const score_rank_node* upper,
    zset_member* members, bool& fragmented) {
    // Fragmented bounds can require extra blob walks at every level. Let
    // the scalar traversal compare each selected key with its ancestors.
    if (node.fragmented) {
        fragmented = true;
        return false;
    }
    score_rank_node left, right;
    if (!load_score_rank_node(node.left, first->rank - base, left)) return false;
    if (left.summary.elements >= node.summary.elements) return fail(status::invalid);
    const auto rank = base + left.summary.elements;
    const auto* middle = std::lower_bound(first, last, rank,
        [](const score_rank_request& request, uint64_t pivot) {
            return request.rank < pivot;
        });
    auto* after = middle;
    while (after != last && after->rank == rank) ++after;
    const auto next_rank = after == last ? UINT64_MAX : after->rank - rank - 1;
    if (!load_score_rank_node(node.right, next_rank, right)) return false;
    uint32_t height;
    uint64_t count;
    int balance;
    if (!summarize(left.summary.height, right.summary.height, left.summary.elements,
        right.summary.elements, height, count, balance)) return false;
    if (node.summary.height != height || node.summary.elements != count ||
        balance < -1 || balance > 1)
        return fail(status::invalid);
    const auto before = [](const score_rank_node& a, const score_rank_node& b) {
        const auto size = std::min(a.key_size, b.key_size);
        const int order = std::memcmp(a.key.data(), b.key.data(), size);
        return order < 0 || (!order && a.key_size < b.key_size);
    };
    if ((lower && !before(*lower, node)) || (upper && !before(node, *upper)))
        return fail(status::invalid);
    if (first != middle && !select_score_paths(left, base, first,
        middle, lower, &node, members, fragmented)) return false;
    if (middle != after) {
        double score;
        if (node.has_value || !zset_page_codec::decode_score_prefix(node.key.data(),
            node.key_size, score)) return fail(status::invalid);
        for (auto* item = middle; item != after; ++item) {
            auto& value = members[item->index];
            value.member.assign(node.key.data() + 8, node.key_size - 8);
            value.score = score;
        }
    }
    return after == last || select_score_paths(right, rank + 1,
        after, last, &node, upper, members, fragmented);
}

bool map_store::check_score_order(const map_blob& key, const map_blob* lower,
    const map_blob* upper) {
    if (key.head && !visit_blob(key,
            [](const map_fragment_view&) { return true; })) return false;
    // Every visited ancestor satisfies its inherited bounds, so these two
    // limits also prove the key is reachable by a lexical search from the root.
    // Compare fragmented bounds in place. Only an emitted key needs owning bytes.
    const auto compare_bound = [&](const map_blob& bound, int& order) {
        if (key.head || bound.head) return compare_blobs(key, bound, order);
        order = key.inline_value.compare(bound.inline_value);
        return true;
    };
    int order;
    if ((lower && (!compare_bound(*lower, order) || order <= 0)) ||
        (upper && (!compare_bound(*upper, order) || order >= 0)))
        return fail(status::invalid);
    return true;
}

bool map_store::visit_score_key(const map_node& node, const visitor& callback) {
    if (node.value.length) return fail(status::invalid);
    std::string fragmented;
    if (node.key.head && !read_blob(node.key, fragmented)) return false;
    const auto& key = node.key.head ? fragmented : node.key.inline_value;
    return callback(key, node.value.inline_value);
}

bool map_store::visit_score_path(const map_node& node, uint64_t first,
    uint64_t last, bool reverse, const map_blob* lower,
    const map_blob* upper, const visitor& callback) {
    node_children children(false);
    if (!check(node, &children) || !check_score_order(node.key, lower, upper)) return false;
    const auto left = node.left ? children.left.elements : 0;
    const auto right = left + 1;
    const auto before = [&] {
        return first >= left || visit_score_path(children.left, first,
            std::min(last, left), reverse, lower, &node.key, callback);
    };
    const auto after = [&] {
        return last <= right || visit_score_path(children.right,
            std::max(first, right) - right, last - right, reverse, &node.key,
            upper, callback);
    };
    if (!(reverse ? after() : before())) return false;
    if (first <= left && left < last && !visit_score_key(node, callback)) return false;
    return reverse ? before() : after();
}

bool map_store::visit_score_interval(const score_prefix_bound& min,
    const score_prefix_bound& max, bool reverse, const visitor& callback) {
    if (!embedded_ || !lexical_ || error_ != status::ok) return fail(status::invalid);
    if (!root_.elements) return root_.tree == 0 || fail(status::invalid);
    map_node root;
    if (!load(root_.tree, root)) return false;
    if (root.elements != root_.elements) return fail(status::invalid);
    return visit_score_interval_path(root, &min, &max, reverse, nullptr, nullptr, callback);
}

bool map_store::visit_score_interval_path(const map_node& node,
    const score_prefix_bound* min, const score_prefix_bound* max, bool reverse,
    const map_blob* lower, const map_blob* upper, const visitor& callback) {
    if (node.key.length < 8) return fail(status::invalid);
    if (!check_score_order(node.key, lower, upper)) return false;
    int order;
    bool below = false, above = false;
    if (min) {
        if (!prefix_order(node.key, min->prefix, order)) return false;
        below = order < 0 || (!order && min->exclusive);
    }
    if (max) {
        if (!prefix_order(node.key, max->prefix, order)) return false;
        above = order > 0 || (!order && max->exclusive);
    }
    node_children children(false);
    if (below || above) {
        // The excluded child still participates in the AVL metadata check.
        // Its key bytes are unnecessary because traversal cannot enter it.
        node_summary left, right;
        if (!check_path(node, above, children, left, right)) return false;
    } else if (!check(node, &children)) return false;
    // Once the current prefix satisfies a bound, the child on the other
    // side of that bound no longer needs to test it.
    const auto before = [&] {
        return below || !node.left || visit_score_interval_path(children.left,
            min, above ? max : nullptr, reverse, lower, &node.key, callback);
    };
    const auto after = [&] {
        return above || !node.right || visit_score_interval_path(children.right,
            below ? min : nullptr, max, reverse, &node.key, upper, callback);
    };
    if (!(reverse ? after() : before())) return false;
    if (!below && !above && !visit_score_key(node, callback)) return false;
    return reverse ? before() : after();
}

bool map_store::visit_rank(uint64_t first, uint64_t count, bool reverse,
    const visitor& callback, bool with_values) {
    if (has_packed_hash()) return with_packed([&](packed_hash_store& store) {
        if (!count || first >= store.size()) return true;
        count = std::min(count,reverse ? first + 1 : store.size() - first);
        for (uint64_t i = 0; i < count; ++i) {
            std::string field,value; bool found;
            if (!store.select(reverse ? first - i : first + i,field,found,
                with_values ? &value : nullptr) || !found) return false;
            if (!callback(field,value)) break;
        }
        return true;
    });
    if (error_ != status::ok) return false;
    if (root_.bucketed)
        return bucket_range(first, count, reverse, callback, with_values);
    if (!count || first >= root_.elements) return true;
    count = std::min(count, reverse ? first + 1 : root_.elements - first);
    rank_cursor reader(*this, reverse);
    if (!reader.seek(first)) return false;
    for (uint64_t i = 0; i < count; ++i) {
        map_node node;
        if (!reader.next(node)) return false;
        std::string key, value;
        if (!read_blob(node.key, key)) return false;
        if (bucket(key) != node.bucket) return fail(status::invalid);
        if (with_values && !read_blob(node.value, value)) return false;
        if (!callback(key, value)) return true;
    }
    return true;
}

} // namespace pkv
