#include "map_store.h"
#include <climits>

namespace pkv {

bool map_store::make_node(const std::string& key, const std::string& value,
    unsigned bucket, uint64_t& id) {
    if (root_.elements == static_cast<uint64_t>(LLONG_MAX))
        return fail(status::too_large);
    map_node node;
    node.bucket = static_cast<uint8_t>(bucket);
    if (!allocate(node.id) || !store_blob(key, map_page_codec::key_inline_bytes,
        node.key) || !store_blob(value, map_page_codec::value_inline_bytes,
            node.value)) return false;
    ++root_.elements;
    if (!write(node)) return false;
    id = node.id;
    return true;
}

bool map_store::rotate(uint64_t& id, bool left, bool intermediate) {
    map_node node, pivot;
    if (!load(id, node)) return false;
    const auto child = left ? node.right : node.left;
    if (!child || !load(child, pivot)) return fail(status::invalid);
    if (left) { node.right = pivot.left; pivot.left = node.id; }
    else { node.left = pivot.right; pivot.right = node.id; }
    int balance;
    if (!refresh(node, balance) || balance < -1 || balance > 1)
        return fail(status::invalid);
    if (!write(node) || !refresh(pivot, balance)) return false;
    // The first half of a double rotation can temporarily leave its new
    // subtree root two levels unbalanced; the outer rotation restores AVL.
    const int limit = intermediate ? 2 : 1;
    if (balance < -limit || balance > limit) return fail(status::invalid);
    if (!write(pivot)) return false;
    id = pivot.id;
    return true;
}

bool map_store::rebalance(map_node& node, uint64_t& id) {
    int balance;
    if (!refresh(node, balance)) return false;
    if (balance < -2 || balance > 2) return fail(status::invalid);
    if (balance == 2 || balance == -2) {
        const bool left = balance == -2;
        auto& child_id = left ? node.right : node.left;
        map_node child;
        int child_balance;
        if (!load(child_id, child) || !refresh(child, child_balance))
            return false;
        if ((left && child_balance > 0) || (!left && child_balance < 0)) {
            if (!rotate(child_id, !left, true)) return false;
        }
        if (!write(node)) return false;
        return rotate(id, left);
    }
    return write(node);
}

bool map_store::insert(uint64_t& id, const std::string& key,
    const std::string& value, bool only_missing, bool& added, unsigned bucket,
    unsigned depth) {
    if (depth >= map_page_codec::max_height) return fail(status::invalid);
    if (!id) {
        if (!make_node(key, value, bucket, id)) return false;
        added = true;
        return true;
    }
    map_node node;
    if (!load(id, node) || !check(node)) return false;
    int order;
    if (!this->order(node, key, bucket, order)) return false;
    if (!order) {
        if (only_missing) return true;
        dirty_ = true;
        if (!erase_blob(node.value) || !store_blob(value,
            map_page_codec::value_inline_bytes, node.value)) return false;
        return write(node);
    }
    if (!insert(order > 0 ? node.left : node.right, key, value, only_missing,
        added, bucket, depth + 1)) return false;
    // Replacing an existing value cannot change ancestors' links or metadata.
    if (!added) return true;
    return rebalance(node, id);
}

bool map_store::remove_min(uint64_t& id, map_node& minimum, unsigned depth) {
    if (!id || depth >= map_page_codec::max_height)
        return fail(status::invalid);
    map_node node;
    if (!load(id, node) || !check(node)) return false;
    if (!node.left) {
        minimum = std::move(node);
        if (!transaction_.erase(owner_, minimum.id))
            return fail(status::storage);
        id = minimum.right;
        return true;
    }
    if (!remove_min(node.left, minimum, depth + 1)) return false;
    return rebalance(node, id);
}

bool map_store::remove(uint64_t& id, const std::string& key, bool& removed,
    unsigned bucket, unsigned depth, std::string* value) {
    if (depth >= map_page_codec::max_height) return fail(status::invalid);
    if (!id) return true;
    map_node node;
    if (!load(id, node) || !check(node)) return false;
    int order;
    if (!this->order(node, key, bucket, order)) return false;
    if (order) {
        if (!remove(order > 0 ? node.left : node.right, key, removed,
            bucket, depth + 1, value)) return false;
        return !removed || rebalance(node, id);
    }
    if (value && !read_blob(node.value, *value)) return false;
    dirty_ = true;
    if (!erase_blob(node.key) || !erase_blob(node.value)) return false;
    if (!node.left || !node.right) {
        if (!transaction_.erase(owner_, node.id)) return fail(status::storage);
        id = node.left ? node.left : node.right;
        --root_.elements;
        removed = true;
        return true;
    }
    map_node successor;
    if (!remove_min(node.right, successor, depth + 1)) return false;
    node.bucket = successor.bucket;
    node.key = std::move(successor.key);
    node.value = std::move(successor.value);
    --root_.elements;
    removed = true;
    return rebalance(node, id);
}

} // namespace pkv
