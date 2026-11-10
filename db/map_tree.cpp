#include "map_store.h"
#include "common/set_sampling.h"
#include <climits>
#include <cstring>

namespace pkv {

bool map_store::make_node(const std::string& key, const std::string& value,
    unsigned bucket, uint64_t& id, node_children& children) {
    if (root_.elements == static_cast<uint64_t>(LLONG_MAX))
        return fail(status::too_large);
    const auto key_limit = key.size() > map_page_codec::key_inline_bytes &&
        key.size() <= map_page_codec::zset_key_inline_bytes && embedded_ &&
        bound_bucket_ == 256 && transaction_.database_.supports_compact_zsets() ?
        map_page_codec::zset_key_inline_bytes : map_page_codec::key_inline_bytes;
    // Fragmented keys cannot consume an inline scratch buffer. Inline keys
    // can now retain it through store_blob rather than allocating a new copy.
    detail::node_key_workspace node(reuse_node_keys(key), key.size() <= key_limit);
    node.bucket = static_cast<uint8_t>(bucket);
    node.id = children.retired_id;
    if (!node.id && !allocate(node.id)) return false;
    if (children.shared_key_head) {
        if (!lexical_ || key.size() <= map_page_codec::zset_key_inline_bytes + 8 ||
            children.shared_key_head > last_id()) return fail(status::invalid);
        node.key.inline_value.assign(key.data(), 8);
        node.key.length = key.size();
        node.key.head = children.shared_key_head;
    } else if (!store_blob(key, key_limit, node.key, children.retired_key_head)) {
        return false;
    }
    if (!value.empty() && !store_blob(value, map_page_codec::value_inline_bytes, node.value))
        return false;
    children.key_head = node.key.head;
    ++root_.elements;
    if (!write(node)) return false;
    id = node.id;
    return true;
}

bool map_store::rotate(map_node& node, map_node& pivot, bool left,
    bool intermediate, node_summary outer, node_summary& pivot_left,
    node_summary& pivot_right) {
    // The caller checked all three subtrees. Only their links change here.
    // Return the changed child summary for the second half of a double rotation.
    auto& inner = left ? pivot_left : pivot_right;
    const auto before = left ? outer : inner;
    const auto after = left ? inner : outer;
    if (left) { node.right = pivot.left; pivot.left = node.id; }
    else { node.left = pivot.right; pivot.right = node.id; }
    int balance;
    if (!summarize(before.height, after.height, before.elements, after.elements,
        node.height, node.elements, balance) || balance < -1 || balance > 1)
        return fail(status::invalid);
    if (!write(node)) return false;
    inner = node_summary(node.height, node.elements);
    if (!summarize(pivot_left.height, pivot_right.height, pivot_left.elements,
        pivot_right.elements, pivot.height, pivot.elements, balance)) return false;
    // The first half of a double rotation can temporarily leave its new
    // subtree root two levels unbalanced; the outer rotation restores AVL.
    const int limit = intermediate ? 2 : 1;
    if (balance < -limit || balance > limit) return fail(status::invalid);
    if (intermediate) {
        // The outer rotation consumes this in-memory pivot and stages its
        // final links. Keep the link checks that encoding would have made.
        return (pivot.left != pivot.id && pivot.right != pivot.id &&
            (!pivot.left || pivot.left != pivot.right)) || fail(status::invalid);
    }
    return write(pivot);
}

bool map_store::rebalance(map_node& node, uint64_t& id, int balance,
    node_summary before, node_summary after, node_summary& updated, bool reuse) {
    if (balance != 2 && balance != -2) return fail(status::invalid);
    const bool left = balance == -2;
    auto& child_id = left ? node.right : node.left;
    if (!child_id || node.left == node.right || node.left == node.id ||
        node.right == node.id) return fail(status::invalid);
    detail::node_key_workspace child(reuse);
    node_summary child_left, child_right;
    int child_balance;
    if (!load(child_id, child) ||
        !stats(child.left, child_left.height, child_left.elements) ||
        !stats(child.right, child_right.height, child_right.elements) ||
        !summarize(child_left.height, child_right.height, child_left.elements,
            child_right.elements, child.height, child.elements, child_balance))
        return false;
    if ((left && child_balance > 0) || (!left && child_balance < 0)) {
        detail::node_key_workspace pivot(reuse);
        node_summary pivot_left, pivot_right;
        if (!load(left ? child.left : child.right, pivot) ||
            !stats(pivot.left, pivot_left.height, pivot_left.elements) ||
            !stats(pivot.right, pivot_right.height, pivot_right.elements) ||
            !rotate(child, pivot, !left, true, left ? child_right : child_left,
                pivot_left, pivot_right)) return false;
        child_id = pivot.id;
        if (node.left == node.right || pivot.id == node.id)
            return fail(status::invalid);
        std::swap(static_cast<map_node&>(child), static_cast<map_node&>(pivot));
        child_left = pivot_left;
        child_right = pivot_right;
    }
    if (!rotate(node, child, left, false, left ? before : after,
        child_left, child_right)) return false;
    id = child.id;
    updated = node_summary(child.height, child.elements);
    return true;
}

bool map_store::insert(uint64_t& id, const std::string& key,
    const std::string& value, bool only_missing, bool& added, unsigned bucket,
    unsigned depth, node_children& children, node_summary& updated,
    const value_gate* gate) {
    const bool reuse = children.reuse();
    if (depth >= map_page_codec::max_height) return fail(status::invalid);
    if (!id) {
        std::string generated;
        const auto* input = &value;
        if (gate && gate->make) {
            if (!gate->make(gate->context, nullptr, generated)) return false;
            input = &generated;
        }
        if (!make_node(key, *input, bucket, id, children))
            return false;
        updated = node_summary(1, 1);
        added = true;
        return true;
    }
    detail::node_key_workspace node(reuse);
    if (!load_child(id, node, children)) return false;
    int order = 0;
    node_summary left, right;
    const bool known = !node.key.head || node.bucket != bucket;
    if (known) {
        // Inline comparison cannot fail or read pages. Only retain an owning
        // copy of the child that insertion may visit.
        if (!this->order(node, key, bucket, order)) return false;
        if (order ? !check_path(node, order > 0, children, left, right) :
            !check(node)) return false;
    } else {
        // Check both child records before following external key fragments.
        if (!check(node, &children)) return false;
        if (node.left)
            left = node_summary(children.left.height, children.left.elements);
        if (node.right)
            right = node_summary(children.right.height, children.right.elements);
    }
    if (!depth && node.elements != root_.elements) return fail(status::invalid);
    if (!known && !this->order(node, key, bucket, order)) return false;
    if (!order) {
        children.key_head = node.key.head;
        if (only_missing) return true;
        bool replace = true;
        if (gate && gate->inspect && !gate->inspect(gate->context, node.value, replace))
            return false;
        if (!replace) return true;
        std::string generated;
        const auto* input = &value;
        if (gate && gate->make) {
            if (!gate->make(gate->context, &node.value, generated)) return false;
            input = &generated;
        }
        dirty_ = true;
        // The owning Hash still publishes the write intent for WATCH. Its
        // field page needs no rewrite when the validated inline bytes match.
        if (!node.value.head && input->size() <= map_page_codec::value_inline_bytes) {
            if (!gate && (bound_bucket_ < 256 ||
                (kind_ == map_kind::hash && !embedded_))) {
                if (bound_bucket_ < 256)
                    if (auto* scope = transaction_.scope_) scope->hash_inline_hit_ = true;
                if (node.value.inline_value == value) return true;
            }
            node.value.inline_value.assign(*input);
            node.value.length = input->size();
            return write(node);
        }
        const auto retired_head = node.value.head;
        const bool same_fragment_reference = retired_head && node.value.length == input->size();
        if (same_fragment_reference && input->size() >
            map_page_codec::max_page_bytes - map_page_codec::fragment_header_bytes)
            return update_fragments(node.value, input);
        if (!erase_blob(node.value) || !store_blob(*input,
            map_page_codec::value_inline_bytes, node.value, retired_head)) return false;
        // The fragment write still publishes through the owning Hash.
        return same_fragment_reference || write(node);
    }
    // Recursion returns the changed subtree's final summary.
    node_summary changed;
    if (!insert(order > 0 ? node.left : node.right, key, value, only_missing,
        added, bucket, depth + 1, children, changed, gate)) return false;
    // Replacing an existing value cannot change ancestors' links or metadata.
    if (!added) return true;
    (order > 0 ? left : right) = changed;
    return rebalance_known(node, id, left, right, order > 0, updated, reuse);
}

bool map_store::rebalance_known(map_node& node, uint64_t& id,
    node_summary left, node_summary right, bool changed_left,
    node_summary& updated, bool reuse, bool same_links) {
    // Re-read a staged sibling: a malformed tree can share descendants.
    const auto sibling = changed_left ? node.right : node.left;
    auto& untouched = changed_left ? right : left;
    if (transaction_.has_staged_bound(entry_, sibling) &&
        !stats(sibling, untouched.height, untouched.elements)) return false;
    const auto previous = node_summary(node.height, node.elements);
    int balance;
    if (!summarize(left.height, right.height, left.elements, right.elements,
        node.height, node.elements, balance)) return false;
    if (balance < -1 || balance > 1)
        return rebalance(node, id, balance, left, right, updated, reuse);
    updated = node_summary(node.height, node.elements);
    if (!same_links || updated.height != previous.height ||
        updated.elements != previous.elements) {
        if (!write(node)) return false;
    }
    return true;
}

template<bool Maximum>
bool map_store::remove_edge(uint64_t& id, map_node& edge, unsigned depth,
    node_children& children, node_summary& updated, std::string* key) {
    const bool reuse = children.reuse();
    if (!id || depth >= map_page_codec::max_height)
        return fail(status::invalid);
    detail::node_key_workspace node(reuse);
    node_summary left, right;
    if (!load_child(id, node, children) ||
        !check_path(node, !Maximum, children, left, right))
        return false;
    if (!depth && node.elements != root_.elements) return fail(status::invalid);
    auto& next = Maximum ? node.right : node.left;
    if (!next) {
        if (key) {
            if (node.value.length) return fail(status::invalid);
            if (!node.key.head) key->swap(node.key.inline_value);
            else if (!read_blob(node.key, *key, true))
                return false;
        }
        edge = std::move(node);
        if (!transaction_.erase_bound(entry_, edge.id))
            return fail(status::storage);
        id = Maximum ? edge.left : edge.right;
        updated = Maximum ? left : right;
        return true;
    }
    if (!remove_edge<Maximum>(next, edge, depth + 1, children,
        Maximum ? right : left, key))
        return false;
    if (key) {
        // The endpoint must also be reachable by the ordinary key search.
        int order;
        if (!this->order(node, *key, 0, order)) return false;
        if (Maximum ? order >= 0 : order <= 0) return fail(status::invalid);
    }
    return rebalance_known(node, id, left, right, !Maximum, updated, reuse);
}

bool map_store::take_edge(bool maximum, std::string& key) {
    if (error_ != status::ok || !embedded_ || !lexical_ || !root_.elements)
        return fail(status::invalid);
    const bool reuse = root_.elements >= 8;
    node_children children(reuse);
    detail::node_key_workspace edge(reuse, false);
    node_summary updated;
    dirty_ = true;
    const bool ok = maximum ?
        remove_edge<true>(root_.tree, edge, 0, children, updated, &key) :
        remove_edge<false>(root_.tree, edge, 0, children, updated, &key);
    if (!ok) return false;
    --root_.elements;
    return root_.elements || clear();
}

bool map_store::join_edge_range(map_node& pivot, uint64_t& id,
    node_summary left, node_summary right, node_summary& summary) {
    const auto delta = static_cast<int>(left.height) - static_cast<int>(right.height);
    if (delta >= -1 && delta <= 1) {
        int balance;
        if (!summarize(left.height, right.height, left.elements, right.elements,
            pivot.height, pivot.elements, balance)) return false;
        id = pivot.id;
        summary = node_summary(pivot.height, pivot.elements);
        return write(pivot);
    }

    // Join down the inner spine of the taller subtree. At each step the
    // joined child can grow by at most one level, as in ordinary insertion.
    const bool taller_left = delta > 0;
    const auto tall = taller_left ? left : right;
    auto& link = taller_left ? pivot.left : pivot.right;
    detail::node_key_workspace node(true);
    node_summary before, after;
    if (!load(link, node) || node.height != tall.height ||
        node.elements != tall.elements)
        return fail(status::invalid);
    detail::node_key_workspace child(true);
    if (!check_path(node, !taller_left, child, before, after)) return false;
    auto& inner = taller_left ? node.right : node.left;
    auto& changed = taller_left ? after : before;
    link = inner;
    if (!join_edge_range(pivot, inner, taller_left ? changed : left,
        taller_left ? right : changed, changed)) return false;
    id = node.id;
    return rebalance_known(node, id, before, after, !taller_left, summary, true);
}

bool map_store::take_edge_range(uint64_t& id, node_summary& summary,
    size_t count, bool maximum, std::vector<std::string>& keys) {
    if (!count) return true;
    detail::node_key_workspace node(true);
    if (!id || !load(id, node) || (summary.height && node.height != summary.height) ||
        node.elements != summary.elements || count > summary.elements)
        return fail(status::invalid);
    node_summary left, right;
    int balance;
    uint32_t height;
    uint64_t elements;
    if (!stats(node.left, left.height, left.elements) ||
        !stats(node.right, right.height, right.elements) ||
        !summarize(left.height, right.height, left.elements, right.elements,
            height, elements, balance) || height != node.height ||
        elements != node.elements || balance < -1 || balance > 1)
        return fail(status::invalid);
    auto& edge = maximum ? right : left;
    auto& far = maximum ? left : right;
    auto& edge_id = maximum ? node.right : node.left;
    auto& far_id = maximum ? node.left : node.right;
    const auto edge_count = static_cast<size_t>(
        std::min<uint64_t>(count, edge.elements));
    if (edge_count && !take_edge_range(edge_id, edge, edge_count, maximum, keys))
        return false;
    if (edge_count == count) {
        int comparison;
        if (!order(node, keys.back(), 0, comparison) ||
            (maximum ? comparison >= 0 : comparison <= 0))
            return fail(status::invalid);
    }
    if (transaction_.has_staged_bound(entry_, far_id)) {
        node_summary actual;
        if (!stats(far_id, actual.height, actual.elements) ||
            actual.height != far.height || actual.elements != far.elements)
            return fail(status::invalid);
    }
    if (edge_count == count)
        return join_edge_range(node, id, left, right, summary);

    if (node.value.length) return fail(status::invalid);
    std::string key;
    if (node.key.head) {
        if (!read_blob(node.key, key, true)) return false;
    } else key.swap(node.key.inline_value);
    if (!keys.empty() && (maximum ? keys.back() <= key : keys.back() >= key))
        return fail(status::invalid);
    keys.push_back(std::move(key));
    if (!transaction_.erase_bound(entry_, node.id)) return fail(status::storage);
    if (!take_edge_range(far_id, far, count - edge_count - 1, maximum, keys))
        return false;
    id = far_id;
    summary = far;
    return true;
}

bool map_store::take_edges(size_t count, bool maximum,
    std::vector<std::string>& keys) {
    if (error_ != status::ok || !embedded_ || !lexical_ ||
        !count || count >= root_.elements) return fail(status::invalid);
    keys.reserve(count);
    // The traversal validates the root's height against its children.
    node_summary root(0, root_.elements);
    if (!take_edge_range(root_.tree, root, count, maximum, keys))
        return fail(status::invalid);
    if (keys.size() != count || root.elements != root_.elements - count)
        return fail(status::invalid);
    root_.elements = root.elements;
    dirty_ = true;
    return true;
}

bool map_store::replace_score_key(const std::string& old_key,
    const std::string& new_key, uint64_t shared_key_head) {
    if (error_ != status::ok || !embedded_ || !lexical_)
        return fail(status::invalid);
    node_children children(reuse_node_keys(old_key));
    children.shared_key_head = shared_key_head;
    node_summary updated;
    return replace_score_path(root_.tree, old_key, new_key, 0, children, updated);
}

bool map_store::replace_score_path(uint64_t& id, const std::string& old_key,
    const std::string& new_key, unsigned depth, node_children& children,
    node_summary& updated) {
    if (!id || depth >= map_page_codec::max_height) return fail(status::invalid);
    const bool reuse = children.reuse();
    detail::node_key_workspace node(reuse);
    if (!load_child(id, node, children)) return false;
    int before = 0, after = 0;
    if (!node.key.head || node.key.shares_fragments()) {
        const auto& pivot = node.key.inline_value;
        auto compare_score_key = [&](const std::string& key, int& result) {
            if (pivot.size() >= 8 && key.size() >= 8) {
                const auto score_order = std::memcmp(pivot.data(), key.data(), 8);
                if (score_order) {
                    result = score_order;
                    return true;
                }
            }
            if (!node.key.head) {
                result = pivot.compare(key);
                return true;
            }
            return compare(node.key, key, result);
        };
        if (!compare_score_key(old_key, before) || !compare_score_key(new_key, after))
            return false;
    }
    if (before && after && (before > 0) == (after > 0)) {
        const bool leftward = before > 0;
        node_summary left, right;
        if (!check_path(node, leftward, children, left, right)) return false;
        if (!depth && node.elements != root_.elements) return fail(status::invalid);
        auto& child = leftward ? node.left : node.right;
        const auto previous = child;
        auto& summary = leftward ? left : right;
        node_summary changed;
        if (!replace_score_path(child, old_key, new_key, depth + 1, children,
            changed)) return false;
        const auto sibling = leftward ? node.right : node.left;
        if (child == previous && summary.height == changed.height &&
            summary.elements == changed.elements &&
            !transaction_.has_staged_bound(entry_, sibling)) {
            updated = node_summary(node.height, node.elements);
            return true;
        }
        summary = changed;
        return rebalance_known(node, id, left, right, leftward, updated, reuse);
    }
    auto empty = [](void* raw, const map_blob& blob, bool&) {
        return blob.length == 0 || static_cast<map_store*>(raw)->fail(status::invalid);
    };
    const value_gate gate{empty, this};
    if (before && after) {
        // The keys lie in opposite branches. Removing from the taller
        // (or equal) branch keeps the final balance within a single AVL
        // rebalance, even if deletion and insertion both change heights.
        const bool old_left = before > 0;
        node_summary left, right;
        if (!check_path(node, old_left, children, left, right)) return false;
        if (!depth && node.elements != root_.elements) return fail(status::invalid);
        auto& old_summary = old_left ? left : right;
        auto& new_summary = old_left ? right : left;
        if (old_summary.height >= new_summary.height) {
            auto& old_child = old_left ? node.left : node.right;
            auto& new_child = old_left ? node.right : node.left;
            const auto previous_left = node.left;
            const auto previous_right = node.right;
            bool removed = false, inserted = false;
            if (!remove(old_child, old_key, removed, 0, depth + 1,
                children, old_summary, nullptr, &gate)) return false;
            if (!removed) return fail(status::invalid);

            children.left.id = children.right.id = 0;
            if (transaction_.has_staged_bound(entry_, new_child)) {
                node_summary actual;
                if (!stats(new_child, actual.height, actual.elements)) return false;
                if (actual.height != new_summary.height ||
                    actual.elements != new_summary.elements)
                    return fail(status::invalid);
            }
            if (!insert(new_child, new_key, "", true, inserted, 0, depth + 1,
                children, new_summary, nullptr)) return false;
            if (!inserted) return fail(status::invalid);
            // The pivot keeps its payload. A balanced split whose child
            // links and total summary stay the same needs no page write.
            return rebalance_known(node, id, left, right, !old_left, updated,
                reuse, node.left == previous_left && node.right == previous_right);
        }
    }
    // A key at the split or a deletion from the shorter branch may
    // require rotating before inserting. Use the ordinary mutations.
    bool removed = false, inserted = false;
    std::swap(static_cast<map_node&>(children.left), static_cast<map_node&>(node));
    children.right.id = 0;
    if (!remove(id, old_key, removed, 0, depth, children, updated, nullptr, &gate))
        return false;
    if (!removed) return fail(status::invalid);
    children.left.id = children.right.id = 0;
    if (!insert(id, new_key, "", true, inserted, 0, depth, children, updated, nullptr))
        return false;
    return inserted || fail(status::invalid);
}

bool map_store::remove(uint64_t& id, const std::string& key, bool& removed,
    unsigned bucket, unsigned depth, node_children& children,
    node_summary& updated, std::string* value,
    const value_gate* gate) {
    const bool reuse = children.reuse();
    if (depth >= map_page_codec::max_height) return fail(status::invalid);
    if (!id) return true;
    detail::node_key_workspace node(reuse);
    if (!load_child(id, node, children)) return false;
    // Inline ordering cannot read pages or fail. External keys retain the
    // original order: validate child metadata before reading their blobs.
    int order = 0;
    const bool known = !node.key.head || node.bucket != bucket;
    if (known && !this->order(node, key, bucket, order)) return false;
    node_summary left, right;
    if (!check_path(node, order > 0, children, left, right)) return false;
    if (!depth && node.elements != root_.elements) return fail(status::invalid);
    bool matched_single_fragment = false;
    if (!known && !compare(node.key, key, order, &matched_single_fragment))
        return false;
    if (order) {
        if (!remove(order > 0 ? node.left : node.right, key, removed,
            bucket, depth + 1, children, order > 0 ? left : right,
            value, gate)) return false;
        return !removed || rebalance_known(node, id, left, right,
            order > 0, updated, reuse);
    }
    bool replace = true;
    if (gate && !gate->inspect(gate->context, node.value, replace))
        return false;
    if (!replace) return true;
    if (value && !read_blob(node.value, *value)) return false;
    removed = remove_node(node, id, depth, children, left, right, updated,
        matched_single_fragment);
    return removed;
}

bool map_store::take_set_rank(uint64_t rank, std::string& key) {
    if (error_ != status::ok || kind_ != map_kind::set || rank >= root_.elements)
        return fail(status::invalid);
    if (root_.bucketed)
        return read_directory(false) && take_bucket_rank(rank, key);
    node_children children(root_.elements >= 8);
    node_summary updated;
    std::string member;
    if (!remove_rank(root_.tree, rank, member, 0, children, updated) ||
        (!root_.elements && !clear())) return false;
    key.swap(member);
    return true;
}

bool map_store::take_bucket_rank(uint64_t rank, std::string& key,
    set_pop_groups* pop) {
    unsigned bucket;
    if (!set_bucket_at_rank(rank, bucket, pop)) return false;
    std::string member;
    auto apply = [&](map_store& child) { return child.take_set_rank(rank, member); };
    if (!bucket_run(bucket, std::ref(apply), false, pop)) return false;
    key.swap(member);
    return true;
}

bool map_store::take_set_sample(std::vector<std::string>& members) {
    if (error_ != status::ok || kind_ != map_kind::set ||
        members.size() > root_.elements) return fail(status::invalid);
    if (members.empty()) return true;
    const bool grouped = root_.grouped && members.size() > 1;
    if (grouped && !read_directory(false)) return false;
    set_pop_groups pop;
    // With at least two draws per directory group on average, keep the
    // changed groups as well as the top counts until the end of the pop.
    if (grouped && members.size() >= 32)
        pop.pages.reset(new std::array<map_group, 16>);
    auto& engine = set_sampling_engine();
    for (auto& member : members) {
        const auto rank = std::uniform_int_distribution<uint64_t>(
            0, root_.elements - 1)(engine);
        if (!(grouped ? take_bucket_rank(rank, member, &pop) :
            take_set_rank(rank, member))) return false;
    }
    return !grouped || !root_.elements || finish_set_pop(pop);
}

bool map_store::remove_rank(uint64_t& id, uint64_t rank, std::string& key,
    unsigned depth, node_children& children, node_summary& updated) {
    if (!id || depth >= map_page_codec::max_height) return fail(status::invalid);
    detail::node_key_workspace node(children.reuse());
    if (!load_child(id, node, children) || !check(node, &children)) return false;
    if ((!depth && node.elements != root_.elements) || rank >= node.elements)
        return fail(status::invalid);
    node_summary left, right;
    if (node.left) left = node_summary(children.left.height, children.left.elements);
    if (node.right) right = node_summary(children.right.height, children.right.elements);
    if (rank == left.elements) {
        if (!read_blob(node.key, key)) return false;
        if (bucket(key) != node.bucket) return fail(status::invalid);
        return remove_node(node, id, depth, children, left, right, updated);
    }
    const bool before = rank < left.elements;
    if (!remove_rank(before ? node.left : node.right,
        before ? rank : rank - left.elements - 1, key, depth + 1, children,
        before ? left : right)) return false;
    // A corrupt ordering must not make a rank-selected member unreachable by key.
    int order;
    if (!this->order(node, key, bucket(key), order)) return false;
    if (before ? order <= 0 : order >= 0) return fail(status::invalid);
    return rebalance_known(node, id, left, right, before, updated, children.reuse());
}

bool map_store::remove_node(map_node& node, uint64_t& id, unsigned depth,
    node_children& children, node_summary left, node_summary right,
    node_summary& updated, bool matched_single_fragment) {
    const bool reuse = children.reuse();
    dirty_ = true;
    // An exact match of the terminal first fragment already checked its
    // identity, offset, length and lack of a successor under this key lock.
    if (children.deferred_key && node.key.head) {
        *children.deferred_key = std::move(node.key);
    } else if (matched_single_fragment) {
        if (!transaction_.erase_bound(entry_, node.key.head))
            return fail(status::storage);
    } else if (!erase_blob(node.key)) return false;
    if (!erase_blob(node.value)) return false;
    // Score replacement can consume the head tombstone in this transaction.
    if (lexical_) children.retired_key_head = node.key.shares_fragments() ? 0 : node.key.head;
    if (!node.left || !node.right) {
        if (!transaction_.erase_bound(entry_, node.id))
            return fail(status::storage);
        children.retired_id = node.id;
        id = node.left ? node.left : node.right;
        updated = node.left ? left : right;
        --root_.elements;
        return true;
    }
    detail::node_key_workspace successor(reuse,
        false);
    if (!remove_edge<false>(node.right, successor, depth + 1, children, right))
        return false;
    children.retired_id = successor.id;
    node.bucket = successor.bucket;
    if (reuse)
        detail::node_key_workspace::recycle(node.key.inline_value);
    node.key = std::move(successor.key);
    node.value = std::move(successor.value);
    --root_.elements;
    return rebalance_known(node, id, left, right, false, updated, reuse);
}

} // namespace pkv
