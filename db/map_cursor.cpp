#include "map_store.h"

namespace pkv {

bool map_store::rank(const std::string& key, uint64_t& rank, bool& found) {
    rank = 0; found = false;
    if (error_ != status::ok || !check_root()) return false;
    uint64_t id = root_.tree, before = 0;
    uint32_t height = map_page_codec::max_height + 1;
    uint64_t elements = root_.elements + 1;
    while (id) {
        map_node node;
        if (!load(id, node) || !check(node)) return false;
        if (node.height >= height || node.elements >= elements)
            return fail(status::invalid);
        uint32_t left_height;
        uint64_t left_count;
        if (!stats(node.left, left_height, left_count)) return false;
        int order;
        if (!this->order(node, key, bucket(key), order)) return false;
        height = node.height; elements = node.elements;
        if (!order) { rank = before + left_count; found = true; return true; }
        if (order > 0) id = node.left;
        else { before += left_count + 1; id = node.right; }
    }
    return true;
}

bool map_store::select(uint64_t rank, std::string& key, bool& found,
    std::string* value) {
    found = false;
    if (error_ != status::ok || !check_root()) return false;
    if (rank >= root_.elements) return true;
    uint64_t id = root_.tree;
    uint32_t height = map_page_codec::max_height + 1;
    uint64_t elements = root_.elements + 1;
    while (id) {
        map_node node;
        if (!load(id, node) || !check(node)) return false;
        if (node.height >= height || node.elements >= elements)
            return fail(status::invalid);
        uint32_t left_height;
        uint64_t left_count;
        if (!stats(node.left, left_height, left_count)) return false;
        height = node.height; elements = node.elements;
        if (rank < left_count) id = node.left;
        else if (rank > left_count) {
            rank -= left_count + 1; id = node.right;
        } else {
            if (!read_blob(node.key, key)) return false;
            if (bucket(key) != node.bucket) return fail(status::invalid);
            if (value && !read_blob(node.value, *value)) return false;
            found = true;
            return true;
        }
    }
    return fail(status::invalid);
}

map_store::cursor::cursor(map_store& store, unsigned first)
: store_(store), first_(first) {}

bool map_store::cursor::descend(uint64_t id, uint32_t height,
    uint64_t elements) {
    while (id) {
        map_node node;
        if (!store_.load(id, node) || !store_.check(node)) return false;
        if (node.height >= height || node.elements >= elements ||
            path_.size() >= map_page_codec::max_height)
            return store_.fail(status::invalid);
        height = node.height; elements = node.elements;
        if (node.bucket >= first_) {
            id = node.left;
            path_.push_back(std::move(node));
        } else id = node.right;
    }
    return true;
}

bool map_store::cursor::next(map_node& node, bool& present) {
    present = false;
    if (store_.error_ != status::ok) return false;
    if (!started_) {
        started_ = true;
        if (!store_.check_root() || !descend(store_.root_.tree,
            map_page_codec::max_height + 1, store_.root_.elements + 1))
            return false;
    }
    if (path_.empty()) return true;
    if (++emitted_ > store_.root_.elements) return store_.fail(status::invalid);
    map_node value = std::move(path_.back());
    path_.pop_back();
    if (!descend(value.right, value.height, value.elements)) return false;
    node = std::move(value); present = true;
    return true;
}

bool map_store::visit(const visitor& callback, bool with_values) {
    if (error_ != status::ok) return false;
    cursor reader(*this);
    map_node node;
    bool present;
    while (reader.next(node, present)) {
        if (!present) return true;
        std::string key, value;
        if (!read_blob(node.key, key)) return false;
        if (bucket(key) != node.bucket) return fail(status::invalid);
        if (with_values && !read_blob(node.value, value)) return false;
        if (!callback(key, value)) return true;
    }
    return false;
}

bool map_store::scan(const scan_options& options, scan_page& out) {
    if (error_ != status::ok) return false;
    if (lexical_) return fail(status::invalid);
    if (!options.count) return fail(status::invalid);
    scan_page page;
    if (options.cursor >= 256) { out = std::move(page); return true; }
    cursor reader(*this, static_cast<unsigned>(options.cursor));
    map_node node;
    bool present;
    uint64_t visited = 0;
    unsigned previous = static_cast<unsigned>(options.cursor);
    while (reader.next(node, present)) {
        if (!present) { out = std::move(page); return true; }
        if (node.bucket < previous) return fail(status::invalid);
        if (node.bucket != previous && visited >= options.count) {
            page.cursor = previous + 1;
            out = std::move(page);
            return true;
        }
        previous = node.bucket;
        ++visited;
        std::string key;
        if (!read_blob(node.key, key)) return false;
        if (bucket(key) != node.bucket) return fail(status::invalid);
        if (!scan_accept(key, options)) continue;
        page.values.push_back(std::move(key));
        if (!options.no_values && kind_ != map_kind::set) {
            std::string value;
            if (!read_blob(node.value, value)) return false;
            page.values.push_back(std::move(value));
        }
    }
    return false;
}

} // namespace pkv
