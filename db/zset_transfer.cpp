#include "zset_store.h"

namespace pkv {

bool zset_store::clone_to(zset_store& destination, bool& copied) {
    copied = false;
    if (error_ != status::ok || destination.error_ != status::ok ||
        !members_) {
        transaction_.abort(); destination.transaction_.abort();
        return false;
    }
    if (&transaction_ == &destination.transaction_ &&
        owner_ == destination.owner_) {
        destination.transaction_.abort(); return fail(status::invalid);
    }
    if (!root_.elements) return true;
    const auto failed = [&] {
        for (auto* context : {this, &destination}) {
            for (const auto* tree : {context->members_.get(),
                context->scores_.get()}) {
                if (tree && tree->error() != status::ok)
                    context->fail(tree->error());
            }
        }
        if (error_ == status::ok) fail(destination.error_ == status::ok ?
            status::invalid : destination.error_);
        if (destination.error_ == status::ok) destination.fail(error_);
        transaction_.abort(); destination.transaction_.abort();
        return false;
    };
    if (!counts() || !members_->check_root() || !scores_->check_root()) {
        return failed();
    }
    destination.initialize(root_.expires_at_ms);
    if (destination.error_ != status::ok) {
        transaction_.abort(); return false;
    }
    destination.root_.last_id = root_.last_id;
    destination.members_->root_ = members_->root_;
    destination.scores_->root_ = scores_->root_;
    map_store::clone_state state;
    if (!members_->clone_node(members_->root_.tree, *destination.members_,
        state, 0) || state.elements != root_.elements) {
        return failed();
    }
    // Share only scalar visited IDs between trees, detecting page aliases
    // across both indexes without keeping their entire member payloads.
    state.previous = map_blob(); state.bucket = 0; state.elements = 0;
    if (!scores_->clone_node(scores_->root_.tree, *destination.scores_,
        state, 0) || state.elements != root_.elements) {
        return failed();
    }
    // Exact cross-index validation uses one owning member at a time. Physical
    // page copying above uses bounded fragments; verification may allocate a
    // large individual member, but never a complete collection snapshot.
    if (!visit([](const std::string&, double) { return true; })) {
        destination.transaction_.abort(); return false;
    }
    if (!destination.counts()) { transaction_.abort(); return false; }
    copied = true;
    return true;
}

} // namespace pkv
