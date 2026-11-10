#include "packed_hash_store.h"
#include "map_store.h"

namespace pkv {

bool map_store::rank(const std::string& key, uint64_t& rank, bool& found) {
    if (has_packed_hash()) return with_packed([&](packed_hash_store& store) { return store.rank(key,rank,found); });
    if (root_.bucketed) return bucket_rank(key, rank, found);
    return rank_in_tree(key, rank, found, false);
}

bool map_store::rank_in_tree(const std::string& key, uint64_t& rank, bool& found,
    bool require_empty) {
    rank = 0; found = false;
    if (error_ != status::ok) return false;
    if ((root_.tree == 0) != (root_.elements == 0)) return fail(status::invalid);
    const auto* scope = transaction_.scope_;
    if (scope && lexical_ && root_.elements > 16 &&
        key.size() <= map_page_codec::key_inline_bytes &&
        !scope->cache_nodes_) {
        bool fallback = false;
        if (!rank_inline(key, rank, found, fallback, require_empty)) return false;
        if (!fallback) return true;
    }
    uint64_t id = root_.tree, before = 0;
    uint32_t height = map_page_codec::max_height + 1;
    uint64_t elements = root_.elements + 1;
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
        int order;
        if (!this->order(node, key, key_bucket, order)) return false;
        height = node.height; elements = node.elements;
        if (!order) {
            if (require_empty && node.value.length) return fail(status::invalid);
            rank = before + left_count; found = true; return true;
        }
        if (order > 0) id = node.left;
        else { before += left_count + 1; id = node.right; }
    }
    return true;
}

bool map_store::select(uint64_t rank, std::string& key, bool& found,
    std::string* value) {
    if (has_packed_hash()) return with_packed([&](packed_hash_store& store) { return store.select(rank,key,found,value); });
    found = false;
    if (root_.bucketed) return bucket_select(rank, key, found, value);
    if (error_ != status::ok) return false;
    if (rank >= root_.elements) return check_root();
    if (!root_.tree) return fail(status::invalid);
    uint64_t id = root_.tree;
    uint32_t height = map_page_codec::max_height + 1;
    uint64_t elements = root_.elements + 1;
    const bool reuse = root_.elements >= 8;
    node_children children(reuse);
    detail::node_key_workspace node(reuse);
    while (id) {
        if (!load_child(id, node, children) || !check(node, &children))
            return false;
        if (node.height >= height || node.elements >= elements ||
            (id == root_.tree && node.elements != root_.elements))
            return fail(status::invalid);
        const uint64_t left_count = node.left ? children.left.elements : 0;
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

bool map_store::select_score_key(uint64_t rank, std::string& key, bool& found) {
    found = false;
    if (error_ != status::ok || !embedded_ || !lexical_) return false;
    if (rank >= root_.elements) return check_root();
    node_children children(root_.elements >= 8);
    if (!select_score_path(root_.tree, rank, key, children, 0)) return false;
    found = true;
    return true;
}

bool map_store::select_score_path(uint64_t id, uint64_t rank, std::string& key,
    node_children& children, unsigned depth) {
    if (!id || depth >= map_page_codec::max_height) return fail(status::invalid);
    detail::node_key_workspace node(children.reuse());
    if (!load_child(id, node, children) || !check(node, &children)) return false;
    if (rank >= node.elements || (!depth && node.elements != root_.elements))
        return fail(status::invalid);
    const auto left = node.left ? children.left.elements : 0;
    if (rank == left) {
        if (node.value.length) return fail(status::invalid);
        return read_blob(node.key, key);
    }
    const bool before = rank < left;
    if (!select_score_path(before ? node.left : node.right,
        before ? rank : rank - left - 1, key, children, depth + 1)) return false;
    // A rank walk must select the same node as a lexical key search.
    int order;
    if (!compare(node.key, key, order)) return false;
    return (before ? order > 0 : order < 0) || fail(status::invalid);
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
    if (has_packed_hash()) return with_packed([&](packed_hash_store& store) {
        packed_hash_store::field_list fields;
        if (!store.all(fields,with_values)) return false;
        for (const auto& item : fields) if (!callback(item.first,item.second)) break;
        return true;
    });
    if (error_ != status::ok) return false;
    if (root_.bucketed) return bucket_visit(callback, with_values);
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

template<class Emit>
bool map_store::visit_subtree(uint64_t id, uint32_t height, uint64_t elements,
    const Emit& emit, node_summary& summary, bool with_values) {
    if (!id) { summary = node_summary(); return true; }
    map_node node;
    if (!load(id, node, with_values)) return false;
    // Reject cycles and malformed depth before descending. Full traversal
    // can validate each summary after reading its children once.
    if (node.height >= height || node.elements >= elements)
        return fail(status::invalid);
    node_summary left, right;
    if (!visit_subtree(node.left, node.height, node.elements, emit, left, with_values))
        return false;
    if (!emit(node)) return false;
    if (!visit_subtree(node.right, node.height, node.elements, emit, right, with_values))
        return false;
    int balance;
    if (!summarize(left.height, right.height, left.elements, right.elements,
        summary.height, summary.elements, balance)) return false;
    return (summary.height == node.height && summary.elements == node.elements &&
        balance >= -1 && balance <= 1) || fail(status::invalid);
}

bool map_store::visit_set_subtree(uint64_t id, uint32_t height, uint64_t elements,
    const member_visitor& callback, node_summary& summary) {
    const auto emit = [&](const map_node& node) {
        if (!node.key.head) return callback(node.key.inline_value);
        std::string key;
        if (!read_blob(node.key, key)) return false;
        if (bucket(key) != node.bucket) return fail(status::invalid);
        return callback(key);
    };
    return visit_subtree(id, height, elements, emit, summary, true);
}

bool map_store::visit_all(const owning_visitor& callback, bool with_values,
    const key_filter* filter) {
    if (has_packed_hash()) return with_packed([&](packed_hash_store& store) {
        packed_hash_store::field_list fields;
        if (!store.all(fields,with_values)) return false;
        for (auto& item : fields) {
            if (filter && !(*filter)(item.first)) continue;
            if (!callback(std::move(item.first),std::move(item.second))) return false;
        }
        return true;
    });
    if (error_ != status::ok) return false;
    if (root_.bucketed) {
        if (!read_directory()) return false;
        const auto scan = [&](map_store& child) {
            return child.visit_all(callback, with_values, filter);
        };
        for (unsigned i = 0; i < 256; ++i) {
            if (index_->directory->buckets[i].elements &&
                !bucket_run(i, std::cref(scan), false)) return false;
        }
        return true;
    }
    const auto emit = [&](map_node& node) {
        auto& key = node.key.inline_value;
        auto& value = node.value.inline_value;
        if (node.key.head) {
            if (!read_blob(node.key, key)) return false;
            if (bucket(key) != node.bucket) return fail(status::invalid);
        }
        if (filter && !(*filter)(key)) return true;
        if (with_values && node.value.head && !read_blob(node.value, value))
            return false;
        return callback(std::move(key), std::move(value));
    };
    node_summary summary;
    if (!visit_subtree(root_.tree, map_page_codec::max_height + 1,
        root_.elements + 1, emit, summary, with_values)) return false;
    return summary.elements == root_.elements || fail(status::invalid);
}

bool map_store::visit_all_set_members(const member_visitor& callback) {
    if (error_ != status::ok) return false;
    if (kind_ != map_kind::set) return fail(status::wrong_kind);
    if (root_.bucketed) {
        if (!read_directory()) return false;
        auto visit_bucket = [&](map_store& child) {
            return child.visit_all_set_members(callback);
        };
        for (unsigned i = 0; i < 256; ++i) {
            if (index_->directory->buckets[i].elements &&
                !bucket_run(i, std::ref(visit_bucket), false)) return false;
        }
        return true;
    }
    node_summary summary;
    if (!visit_set_subtree(root_.tree, map_page_codec::max_height + 1,
        root_.elements + 1, callback, summary)) return false;
    return summary.elements == root_.elements || fail(status::invalid);
}

bool map_store::visit_set_bucket(unsigned index, const member_visitor& callback) {
    if (error_ != status::ok) return false;
    if (kind_ != map_kind::set || !root_.grouped || index >= 256)
        return fail(status::invalid);
    if (!probe_directory_ready_) {
        if (!read_directory(false)) return false;
        probe_directory_ready_ = true;
    }
    if (!read_group(index / 16)) return false;
    const auto bucket = index_->group.buckets[index % 16];
    if (!bucket.elements) return true;
    struct restore_bucket {
        unsigned& bound;
        unsigned previous;
        ~restore_bucket() { bound = previous; }
    } restore{bound_bucket_, bound_bucket_};
    bound_bucket_ = index;
    node_summary summary;
    if (!visit_set_subtree(bucket.tree, map_page_codec::max_height + 1,
        bucket.elements + 1, callback, summary)) return false;
    return summary.elements == bucket.elements || fail(status::invalid);
}

bool map_store::scan(const scan_options& options, scan_page& out) {
    if (has_packed_hash()) return with_packed([&](packed_hash_store& store) { return store.scan(options,out); });
    if (error_ != status::ok) return false;
    if (lexical_) return fail(status::invalid);
    if (!options.count) return fail(status::invalid);
    if (root_.bucketed) return bucket_scan(options, out);
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
