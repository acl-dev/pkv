#include "map_store.h"

namespace pkv {

bool map_store::bucket_clone(map_store& destination, bool& copied) {
    copied = false;
    if (!read_directory()) { destination.transaction_.abort(); return false; }
    destination.initialize(root_.expires_at_ms);
    if (destination.error_ != status::ok) {
        transaction_.abort(); return false;
    }
    destination.root_ = root_;
    clone_state state;
    state.limit = root_.elements;
    state.visited.insert(root_.tree);
    if (root_.grouped) {
        for (const auto& parent : index_->top.buckets) {
            if (!parent.tree) continue;
            if (!state.visited.insert(parent.tree).second)
                return fail(status::invalid);
            std::string bytes;
            if (!read(parent.tree, bytes, true) ||
                !destination.transaction_.put_bound(destination.entry_,
                    parent.tree, std::move(bytes))) {
                destination.transaction_.abort(); return fail(status::storage);
            }
        }
    }
    for (unsigned i = 0; i < 256; ++i) {
        const auto& entry = index_->directory->buckets[i];
        if (!entry.elements) continue;
        map_root metadata;
        metadata.kind = kind_;
        metadata.tree = entry.tree; metadata.elements = entry.elements;
        metadata.last_id = root_.last_id;
        map_store child(transaction_, entry_, metadata, root_.last_id, false);
        child.bound_bucket_ = i;
        if (!child.open() || !child.check_root() ||
            !child.clone_node(entry.tree, destination, state, 0)) {
            if (error_ == status::ok) fail(child.error_ == status::ok ?
                status::invalid : child.error_);
            destination.transaction_.abort(); return false;
        }
    }
    if (state.elements != root_.elements) {
        destination.transaction_.abort(); return fail(status::invalid);
    }
    std::string bytes;
    const auto encoded = root_.grouped ?
        map_page_codec::encode_group(root_, 256, index_->top, bytes) :
        map_page_codec::encode_directory(root_, *index_->directory, bytes);
    if (!decoded(encoded) || !destination.transaction_.put_bound(
        destination.entry_, root_.tree, std::move(bytes))) {
        destination.transaction_.abort(); return fail(status::storage);
    }
    copied = true;
    return true;
}

} // namespace pkv
