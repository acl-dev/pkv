#include "map_store.h"

namespace pkv {
bool map_store::read_group_page(const map_root& root, unsigned tag,
    map_group& out, bool& cached_copy) {
    cached_copy = false;
    detail::map_group_cache::entry* cached = nullptr;
    auto* scope = transaction_.scope_;
    if (scope && !transaction_.failed_ && !transaction_.has_changes() &&
        !entry_->clear_pages && entry_->action != db::update_action::erase) {
        if (!scope->map_groups_)
            scope->map_groups_.reset(new (std::nothrow) detail::map_group_cache);
        if (scope->map_groups_) {
            cached = &scope->map_groups_->groups[tag == 256 ? 16 : tag];
            if (cached->id == root.tree && cached->elements == root.elements &&
                cached->last_id <= root.last_id) {
                out = cached->group;
                cached_copy = true;
                return true;
            }
        }
    }
    detail::node_read_workspace workspace;
    auto& bytes = workspace.buffer();
    struct state {
        const map_root& root; unsigned tag; map_group& out;
        map_page_status result;
    } value{root, tag, out, map_page_status::invalid};
    auto decode = [](void* raw, const std::string& encoded) {
        auto& job = *static_cast<state*>(raw);
        job.result = map_page_codec::decode_group(encoded, job.root,
            job.tag, job.out);
    };
    if (!inspect(root.tree, bytes, decode, &value) || !decoded(value.result))
        return false;
    if (cached) {
        cached->id = root.tree;
        cached->elements = root.elements;
        cached->last_id = root.last_id;
        cached->group = out;
        auto& validated = scope->map_groups_->validated_groups;
        if (tag == 256) validated = 0;
        else validated &= ~(1U << tag);
        cached_copy = true;
    }
    return true;
}

bool map_store::write_group_page(const map_root& root, unsigned tag,
    const map_group& group) {
    std::string bytes;
    if (!decoded(map_page_codec::encode_group(root, tag, group, bytes)))
        return false;
    if (!transaction_.put_bound(entry_, root.tree, std::move(bytes)))
        return fail(status::storage);
    return true;
}

bool map_store::read_group(unsigned index, const set_pop_groups* pop) {
    if (!index_ || index >= 16) return fail(status::invalid);
    if (index_->group_index == index) return true;
    const auto& entry = index_->top.buckets[index];
    if (!entry.elements) index_->group = map_group();
    else if (pop && (pop->changed & (1U << index)))
        index_->group = (*pop->pages)[index];
    else {
        auto state = root_;
        state.tree = entry.tree; state.elements = entry.elements;
        bool cached_copy;
        if (!read_group_page(state, index, index_->group, cached_copy)) return false;
        auto* cache = cached_copy && index_->top_cached ?
            transaction_.scope_->map_groups_.get() : nullptr;
        const auto mask = 1U << index;
        if (cache && (cache->validated_groups & mask)) {
            index_->group_index = index;
            return true;
        }
        // Most bucket IDs do not share even the low byte of a parent ID.
        // Resolve collisions with the existing exact comparison.
        uint64_t parents[4] = {};
        for (const auto& parent : index_->top.buckets)
            parents[(parent.tree >> 6) & 3] |= uint64_t(1) << (parent.tree & 63);
        for (const auto& bucket : index_->group.buckets) {
            if (!bucket.tree) continue;
            if (bucket.tree == root_.tree) return fail(status::invalid);
            if (!(parents[(bucket.tree >> 6) & 3] &
                (uint64_t(1) << (bucket.tree & 63)))) continue;
            for (const auto& parent : index_->top.buckets)
                if (bucket.tree == parent.tree) return fail(status::invalid);
        }
        if (cache) cache->validated_groups |= mask;
    }
    index_->group_index = index;
    return true;
}

bool map_store::update_group(unsigned index, const map_root& state,
    uint64_t old_count, set_pop_groups* pop) {
    index_->top_cached = false;
    auto& entry = index_->group.buckets[index % 16];
    entry.tree = state.tree; entry.elements = state.elements;
    if (index_->directory) index_->directory->buckets[index] = entry;
    auto& parent = index_->top.buckets[index / 16];
    parent.elements = parent.elements - old_count + state.elements;
    if (!parent.elements) {
        if (!transaction_.erase_bound(entry_, parent.tree))
            return fail(status::storage);
        parent.tree = 0;
    } else {
        if (!parent.tree && !allocate(parent.tree)) return false;
        if (pop && pop->pages) {
            const unsigned group = index / 16;
            auto& page = (*pop->pages)[group];
            if (pop->changed & (1U << group)) page.buckets[index % 16] = entry;
            else {
                page = index_->group;
                pop->changed |= 1U << group;
            }
        } else {
            auto metadata = root_;
            metadata.tree = parent.tree; metadata.elements = parent.elements;
            if (!write_group_page(metadata, index / 16, index_->group))
                return false;
        }
    }
    return pop || write_group_page(root_, 256, index_->top);
}

bool map_store::finish_set_pop(const set_pop_groups& pop) {
    for (unsigned group = 0; group < 16; ++group) {
        if (!(pop.changed & (1U << group))) continue;
        const auto& entry = index_->top.buckets[group];
        if (!entry.elements) continue;
        auto metadata = root_;
        metadata.tree = entry.tree; metadata.elements = entry.elements;
        if (!write_group_page(metadata, group, (*pop.pages)[group])) return false;
    }
    return write_group_page(root_, 256, index_->top);
}

bool map_store::write_groups() {
    if (!index_ || !index_->directory) return fail(status::invalid);
    index_->top_cached = false;
    index_->top = map_group();
    for (unsigned i = 0; i < 16; ++i) {
        map_group group;
        auto metadata = root_;
        metadata.elements = 0;
        for (unsigned j = 0; j < 16; ++j) {
            group.buckets[j] = index_->directory->buckets[i * 16 + j];
            metadata.elements += group.buckets[j].elements;
        }
        if (!metadata.elements) continue;
        if (!allocate(metadata.tree)) return false;
        metadata.last_id = root_.last_id;
        if (!write_group_page(metadata, i, group)) return false;
        index_->top.buckets[i].tree = metadata.tree;
        index_->top.buckets[i].elements = metadata.elements;
    }
    index_->group_index = 16;
    return write_group_page(root_, 256, index_->top);
}
} // namespace pkv
