#include "map_store.h"

namespace pkv {

bool map_store::clone_blob(const map_blob& blob, map_store& destination,
    clone_state& state, unsigned* bucket) {
    if (!blob.head) {
        if (bucket) *bucket = this->bucket(blob.inline_value);
        return true;
    }
    if (blob.shares_fragments()) {
        if (!lexical_ || !state.shareable_heads.erase(blob.head))
            return fail(status::invalid);
        if (bucket) *bucket = 0;
        return visit_blob(blob, [](const map_fragment_view&) { return true; });
    }
    const bool crc = uses_crc_bucket(blob.length);
    auto hash = crc ? 0 : detail::scan_bucket_seed;
    const bool ok = visit_blob(blob, [&](const map_fragment_view& page) {
        if (!state.visited.insert(page.id).second) return fail(status::invalid);
        if (bucket && !lexical_)
            hash = crc ? update_crc_bucket(hash, page.data, page.size) :
                detail::scan_bucket_update(page.data, page.size, hash);
        std::string bytes;
        if (!destination.decoded(map_page_codec::encode_fragment(
            page.id, page.next, page.offset, page.data, page.size, bytes)))
            return false;
        return destination.transaction_.put_bound(destination.entry_, page.id,
            std::move(bytes)) || destination.fail(status::storage);
    });
    if (!ok || destination.error_ != status::ok) return false;
    if (bucket) *bucket = lexical_ ? 0 : static_cast<unsigned>(hash & 255);
    return true;
}

bool map_store::clone_node(uint64_t id, map_store& destination,
    clone_state& state, unsigned depth) {
    if (!id) return true;
    if (depth >= map_page_codec::max_height ||
        !state.visited.insert(id).second) return fail(status::invalid);
    map_node node;
    if (!load(id, node) || !check(node) ||
        !clone_node(node.left, destination, state, depth + 1)) return false;
    if (state.elements) {
        if (node.bucket < state.bucket) return fail(status::invalid);
        if (node.bucket == state.bucket) {
            int order;
            if (!compare_blobs(state.previous, node.key, order)) return false;
            if (order >= 0) return fail(status::invalid);
        }
    }
    unsigned bucket;
    if (!clone_blob(node.key, destination, state, &bucket)) return false;
    if (bucket != node.bucket) return fail(status::invalid);
    if (state.track_shared_fragments && !lexical_ && node.key.head)
        state.shareable_heads.insert(node.key.head);
    if (!clone_blob(node.value, destination, state)) return false;
    state.previous = node.key; state.bucket = node.bucket;
    if (++state.elements > state.limit) return fail(status::invalid);
    if (!destination.write(node)) return false;
    return clone_node(node.right, destination, state, depth + 1);
}

bool map_store::clone_to(map_store& destination, bool& copied) {
    copied = false;
    if (embedded_ || destination.embedded_) {
        destination.transaction_.abort(); return fail(status::invalid);
    }
    if (error_ != status::ok || destination.error_ != status::ok) {
        transaction_.abort(); destination.transaction_.abort();
        return false;
    }
    if (&transaction_ == &destination.transaction_ &&
        owner_ == destination.owner_) {
        destination.transaction_.abort();
        return fail(status::invalid);
    }
    if (kind_ != destination.kind_) {
        destination.fail(status::wrong_kind);
        return fail(status::wrong_kind);
    }
    if (!root_.elements) return true;
    if (has_packed_hash()) return clone_packed_hash(destination,copied);
    if (root_.bucketed) return bucket_clone(destination, copied);
    if (!check_root()) { destination.transaction_.abort(); return false; }
    destination.initialize(root_.expires_at_ms);
    if (destination.error_ != status::ok) {
        transaction_.abort(); return false;
    }
    destination.root_ = root_;
    clone_state state;
    state.limit = root_.elements;
    if (!clone_node(root_.tree, destination, state, 0) ||
        state.elements != root_.elements) {
        transaction_.abort(); destination.transaction_.abort();
        return false;
    }
    copied = true;
    return true;
}

} // namespace pkv
