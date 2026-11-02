#include "map_store.h"
#include <algorithm>
#include <cstring>

namespace pkv {

bool map_store::prefix_order(const map_blob& key, const std::string& input,
    int& order) {
    order = 0;
    blob_reader reader(*this, key);
    size_t offset = 0;
    while (offset < input.size()) {
        map_fragment page;
        bool present;
        if (!reader.next(page, present)) return false;
        if (!present) { order = -1; return true; }
        const auto length = std::min(page.bytes.size(), input.size() - offset);
        order = std::memcmp(page.bytes.data(), input.data() + offset, length);
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
    if (!check_root()) return false;
    uint64_t id = root_.tree, before = 0, elements = root_.elements + 1;
    uint32_t height = map_page_codec::max_height + 1;
    while (id) {
        map_node node;
        if (!load(id, node) || !check(node)) return false;
        if (node.height >= height || node.elements >= elements)
            return fail(status::invalid);
        uint32_t left_height;
        uint64_t left_count;
        if (!stats(node.left, left_height, left_count)) return false;
        int comparison;
        if (!(prefix ? prefix_order(node.key, key, comparison) :
            order(node, key, bucket(key), comparison))) return false;
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
    if (!store_.check_root() || rank >= store_.root_.elements)
        return store_.fail(status::invalid);
    path_.clear();
    uint64_t id = store_.root_.tree, elements = store_.root_.elements + 1;
    uint32_t height = map_page_codec::max_height + 1;
    while (id) {
        map_node node;
        if (!store_.load(id, node) || !store_.check(node)) return false;
        if (node.height >= height || node.elements >= elements)
            return store_.fail(status::invalid);
        uint32_t left_height;
        uint64_t left_count;
        if (!store_.stats(node.left, left_height, left_count)) return false;
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
    while (id) {
        map_node node;
        if (!store_.load(id, node) || !store_.check(node)) return false;
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

bool map_store::visit_rank(uint64_t first, uint64_t count, bool reverse,
    const visitor& callback, bool with_values) {
    if (error_ != status::ok) return false;
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
