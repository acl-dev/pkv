#include "map_store.h"
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

bool map_store::visit_score_path(const map_node& node, uint64_t first,
    uint64_t last, bool reverse, const std::string* lower,
    const std::string* upper, const visitor& callback) {
    node_children children(false);
    if (!check(node, &children)) return false;
    std::string fragmented;
    if (node.key.head && !read_blob(node.key, fragmented)) return false;
    const auto& key = node.key.head ? fragmented : node.key.inline_value;
    // Every visited ancestor satisfies its inherited bounds, so these two
    // limits also prove the key is reachable by a lexical search from the root.
    if ((lower && key <= *lower) || (upper && key >= *upper))
        return fail(status::invalid);
    const auto left = node.left ? children.left.elements : 0;
    const auto right = left + 1;
    const auto before = [&] {
        return first >= left || visit_score_path(children.left, first,
            std::min(last, left), reverse, lower, &key, callback);
    };
    const auto after = [&] {
        return last <= right || visit_score_path(children.right,
            std::max(first, right) - right, last - right, reverse, &key,
            upper, callback);
    };
    if (!(reverse ? after() : before())) return false;
    if (first <= left && left < last) {
        if (node.value.length) return fail(status::invalid);
        if (!callback(key, node.value.inline_value)) return false;
    }
    return reverse ? before() : after();
}

bool map_store::visit_rank(uint64_t first, uint64_t count, bool reverse,
    const visitor& callback, bool with_values) {
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
