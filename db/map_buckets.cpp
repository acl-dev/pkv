#include "map_store.h"
#include <algorithm>
#include <array>
#include <climits>
#include <limits>

namespace pkv {

bool map_store::read_directory(bool full) {
    if (error_ != status::ok) return false;
    if (!root_.bucketed) return fail(status::invalid);
    index_.acquire();
    if (root_.grouped) {
        index_->group_index = 16;
        if (!read_group_page(root_, 256, index_->top, index_->top_cached))
            return false;
        if (!full) return true;
        if (!index_->directory) index_->directory.reset(new map_directory);
        std::unordered_set<uint64_t> seen;
        for (unsigned i = 0; i < 16; ++i) {
            if (!read_group(i)) return false;
            for (unsigned j = 0; j < 16; ++j) {
                const auto& entry = index_->group.buckets[j];
                if (entry.tree && !seen.insert(entry.tree).second)
                    return fail(status::invalid);
                index_->directory->buckets[i * 16 + j] = entry;
            }
        }
        return true;
    }
    if (!index_->directory) index_->directory.reset(new map_directory);
    detail::node_read_workspace workspace;
    auto& bytes = workspace.buffer();
    return read(root_.tree, bytes, true) &&
        decoded(map_page_codec::decode_directory(bytes, root_,
            *index_->directory));
}

bool map_store::write_directory() {
    if (root_.grouped) return write_groups();
    std::string bytes;
    if (!index_ || !index_->directory) return fail(status::invalid);
    if (!decoded(map_page_codec::encode_directory(root_,
        *index_->directory, bytes))) return false;
    return transaction_.put_bound(entry_, root_.tree, std::move(bytes)) ||
        fail(status::storage);
}

bool map_store::bucket_run(unsigned index, const bucket_operation& callback,
    bool refresh, set_pop_groups* pop) {
    if (error_ != status::ok) return false;
    if (index >= 256 || !root_.bucketed) return fail(status::invalid);
    const auto group = index / 16;
    const auto* scope = transaction_.scope_;
    const map_group* cached_top = nullptr;
    const map_group* cached_group = nullptr;
    if (refresh && root_.grouped && scope &&
        scope->map_groups_ && !transaction_.failed_ && !transaction_.has_changes() &&
        !entry_->clear_pages && entry_->action != db::update_action::erase) {
        const auto& cache = *scope->map_groups_;
        const auto& top = cache.groups[16];
        const auto& child = cache.groups[group];
        const auto& parent = top.group.buckets[group];
        if ((cache.validated_groups & (1U << group)) &&
            top.id == root_.tree && top.elements == root_.elements &&
            top.last_id <= root_.last_id && child.id == parent.tree &&
            child.elements == parent.elements && child.last_id <= root_.last_id) {
            cached_top = &top.group;
            cached_group = &child.group;
        }
    }
    if (!cached_group) {
        if (refresh && !read_directory(false)) return false;
        if (root_.grouped && !read_group(group, pop)) return false;
        if (!root_.grouped && !index_->directory) return fail(status::invalid);
    }
    const auto& entry = cached_group ? cached_group->buckets[index % 16] :
        root_.grouped ? index_->group.buckets[index % 16] :
                       index_->directory->buckets[index];
    const auto old_tree = entry.tree, old_count = entry.elements;
    map_root state;
    state.kind = kind_;
    state.tree = old_tree; state.elements = old_count;
    state.last_id = root_.last_id;
    map_store child(transaction_, entry_, state, root_.last_id, false);
    child.bound_bucket_ = index;
    if (!child.open() || !callback(child))
        return fail(child.error_ == status::ok ? status::storage :
            child.error_);
    const bool changed = child.dirty_;
    if (!child.finish()) return fail(child.error_);
    if (!changed) return true;
    if (state.elements > old_count && state.elements - old_count >
        static_cast<uint64_t>(LLONG_MAX) - root_.elements)
        return fail(status::too_large);
    root_.elements = root_.elements - old_count + state.elements;
    dirty_ = true;
    if (!root_.elements) return clear();
    if (root_.grouped) {
        if (old_tree == state.tree && old_count == state.elements) return true;
        if (cached_group) {
            // Only structural changes need an owned directory to edit.
            index_.acquire();
            index_->top = *cached_top;
            index_->group = *cached_group;
            index_->group_index = group;
        }
        return update_group(index, state, old_count, pop);
    }
    auto& updated = index_->directory->buckets[index];
    updated.tree = state.tree; updated.elements = state.elements;
    if (old_tree != updated.tree || old_count != updated.elements)
        return write_directory();
    return true;
}

bool map_store::rebuild_bucket(std::vector<map_node>& nodes, size_t first,
    size_t end, uint64_t& tree, uint32_t& height) {
    tree = 0; height = 0;
    if (first == end) return true;
    const size_t middle = first + (end - first) / 2;
    auto& node = nodes[middle];
    uint32_t left, right;
    if (!rebuild_bucket(nodes, first, middle, node.left, left) ||
        !rebuild_bucket(nodes, middle + 1, end, node.right, right))
        return false;
    node.height = std::max(left, right) + 1;
    node.elements = end - first;
    if (!write(node)) return false;
    tree = node.id; height = node.height;
    return true;
}

bool map_store::build_tree(const bulk_member* members, size_t count,
    uint64_t& tree, uint32_t& height, map_node& node) {
    const bool compact_leaf = kind_ == map_kind::set &&
        transaction_.database_.supports_compact_set_leaves();
    return build_tree_from(0, count, tree, height, node,
        [&](size_t index, map_node& current) {
            const auto& member = members[index];
            current.bucket = static_cast<uint8_t>(member.bucket);
            return store_bulk_blob(*member.key, map_page_codec::key_inline_bytes,
                current.key) && (!member.value || store_bulk_blob(*member.value,
                    map_page_codec::value_inline_bytes, current.value));
        }, compact_leaf);
}

bool map_store::build_set(const set_member_at& member_at, size_t count,
    long long& added) {
    added = 0;
    if (kind_ != map_kind::set || embedded_ || exists() || !count)
        return fail(status::invalid);
    std::vector<bulk_member> ordered;
    if (count > ordered.max_size() || count > static_cast<uint64_t>(LLONG_MAX))
        return fail(status::too_large);
    ordered.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        const auto& member = member_at(i);
        if (member.size() > map_page_codec::max_payload_bytes)
            return fail(status::too_large);
        ordered.push_back({&member, nullptr, bucket(member)});
    }
    if (!build_ordered(ordered)) return false;
    added = static_cast<long long>(ordered.size());
    return true;
}

bool map_store::build_hash(const hash_field_refs& fields, int& added) {
    added = 0;
    if (kind_ != map_kind::hash || embedded_ || exists() || fields.empty())
        return fail(status::invalid);
    std::vector<bulk_member> ordered;
    if (fields.size() > ordered.max_size() || fields.size() > INT_MAX)
        return fail(status::too_large);
    ordered.reserve(fields.size());
    // Stable sorting in reverse request order keeps the last duplicate.
    for (auto it = fields.rbegin(); it != fields.rend(); ++it) {
        const auto& field = *it;
        ordered.push_back({field.first, field.second, bucket(*field.first)});
    }
    if (!build_ordered(ordered)) return false;
    added = static_cast<int>(ordered.size());
    return true;
}

bool map_store::build_ordered(std::vector<bulk_member>& ordered) {
    if (kind_ == map_kind::hash) {
        std::stable_sort(ordered.begin(), ordered.end(), bulk_member::less);
    } else if (ordered.size() < set_partition_size) {
        std::sort(ordered.begin(), ordered.end(), bulk_member::less);
    } else {
        sort_member_buckets(ordered, [](const bulk_member& a, const bulk_member& b) {
            return *a.key < *b.key;
        });
    }
    ordered.erase(std::unique(ordered.begin(), ordered.end(),
        [](const bulk_member& a, const bulk_member& b) {
            return *a.key == *b.key;
        }), ordered.end());
    const bool grouped = adaptive_ && ordered.size() >=
        (kind_ == map_kind::hash ? hash_partition_size : set_partition_size);
    constexpr auto fragment_capacity = map_page_codec::max_page_bytes -
        map_page_codec::fragment_header_bytes;
    const auto fragments = [](size_t size, size_t inline_limit) {
        return size > inline_limit ? 1 + (size - 1) / fragment_capacity : 0;
    };
    size_t pages = ordered.size() + (grouped ? 17 : 0);
    for (const auto& field : ordered) {
        const auto key = field.key->size();
        const auto value = field.value ? field.value->size() : 0;
        if (key > map_page_codec::max_payload_bytes || value > map_page_codec::max_payload_bytes)
            return fail(status::too_large);
        const auto extra = fragments(key, map_page_codec::key_inline_bytes) +
            fragments(value, map_page_codec::value_inline_bytes);
        if (extra > SIZE_MAX - pages) return fail(status::too_large);
        pages += extra;
    }

    if (!transaction_.reset_pages(owner_)) return fail(status::storage);
    root_.elements = ordered.size();
    transaction_.reserve_sorted_pages(entry_, pages);
    // The recursive build writes one node at a time, so it needs one payload buffer.
    map_node node;
    if (!grouped) {
        uint32_t height;
        if (!build_tree(ordered.data(), ordered.size(), root_.tree, height, node))
            return false;
    } else {
        root_.bucketed = true; root_.grouped = true;
        auto& index = index_.acquire();
        index.directory.reset(new map_directory);
        size_t first = 0;
        while (first < ordered.size()) {
            const auto bucket = ordered[first].bucket;
            size_t end = first + 1;
            while (end < ordered.size() && ordered[end].bucket == bucket) ++end;
            auto& entry = index.directory->buckets[bucket];
            entry.elements = end - first;
            uint32_t height;
            if (!build_tree(ordered.data() + first, end - first,
                entry.tree, height, node)) return false;
            first = end;
        }
        if (!write_groups()) return false;
    }
    return true;
}

bool map_store::partition() {
    if (has_packed_hash()) return true;
    if (error_ != status::ok) return false;
    if (embedded_) return fail(status::wrong_kind);
    if (root_.bucketed || !root_.elements) return true;
    if (root_.last_id > std::numeric_limits<uint64_t>::max() - 17)
        return fail(status::too_large);
    std::vector<map_node> nodes;
    if (root_.elements > nodes.max_size()) return fail(status::too_large);
    nodes.reserve(static_cast<size_t>(root_.elements));
    std::unordered_set<uint64_t> seen;
    cursor reader(*this);
    map_node node;
    bool present;
    while (reader.next(node, present)) {
        if (!present) break;
        if (!seen.insert(node.id).second) return fail(status::invalid);
        if (!nodes.empty()) {
            const auto& previous = nodes.back();
            if (node.bucket < previous.bucket) return fail(status::invalid);
            if (node.bucket == previous.bucket) {
                int order;
                if (!compare_blobs(previous.key, node.key, order)) return false;
                if (order >= 0) return fail(status::invalid);
            }
        }
        nodes.push_back(std::move(node));
    }
    if (error_ != status::ok) return false;
    if (nodes.size() != root_.elements) return fail(status::invalid);
    std::unique_ptr<map_directory> directory(new map_directory);
    uint64_t id;
    if (!allocate(id)) return false;
    size_t first = 0;
    for (unsigned bucket = 0; bucket < 256; ++bucket) {
        size_t end = first;
        while (end < nodes.size() && nodes[end].bucket == bucket) ++end;
        auto& entry = directory->buckets[bucket];
        uint32_t height;
        if (!rebuild_bucket(nodes, first, end, entry.tree, height))
            return false;
        entry.elements = end - first;
        first = end;
    }
    if (first != nodes.size()) return fail(status::invalid);
    root_.tree = id; root_.bucketed = true; root_.grouped = true;
    index_.acquire().directory = std::move(directory);
    return write_directory();
}

} // namespace pkv
