#include "zset_store.h"
#include <cmath>

namespace pkv {

zset_store::zset_store(page_transaction& transaction, const std::string& owner)
: transaction_(transaction), owner_(owner)
, entry_(transaction.roots().find(owner)) {}

zset_store::~zset_store() {
    if (dirty_) transaction_.abort();
}

bool zset_store::fail(status error) {
    if (error_ == status::ok) error_ = error;
    if (error != status::not_paged) transaction_.abort();
    return false;
}

bool zset_store::tree_error() {
    for (const auto* tree : {members_.get(), scores_.get()}) {
        if (tree && tree->error() != status::ok) return fail(tree->error());
    }
    return fail(status::invalid);
}

bool zset_store::trees() {
    member_state_ = map_root(); score_state_ = map_root();
    member_state_.tree = root_.members; score_state_.tree = root_.scores;
    member_state_.elements = score_state_.elements = root_.elements;
    member_state_.last_id = score_state_.last_id = root_.last_id;
    members_.reset(new map_store(transaction_, owner_, member_state_,
        root_.last_id, false));
    scores_.reset(new map_store(transaction_, owner_, score_state_,
        root_.last_id, true));
    return (members_->open() && scores_->open()) || tree_error();
}

bool zset_store::open() {
    if (!entry_ || members_) return fail(status::invalid);
    if ((entry_->found || entry_->action == db::update_action::write) &&
        entry_->action != db::update_action::erase) {
        if (entry_->value.compare(0, 3, "PZR") != 0)
            return fail(status::not_paged);
        const auto result = zset_page_codec::decode_root(entry_->value, root_);
        if (result != map_page_status::ok) return fail(result ==
            map_page_status::unsupported ? status::unsupported :
            status::invalid);
        if (!root_.elements) return fail(status::invalid);
    }
    return trees();
}

void zset_store::initialize(int64_t expires_at_ms) {
    if (!entry_ || expires_at_ms < -1) { fail(status::invalid); return; }
    if (!transaction_.reset_pages(owner_)) { fail(status::storage); return; }
    root_ = zset_root(); root_.expires_at_ms = expires_at_ms;
    entry_->action = db::update_action::write;
    error_ = status::ok; dirty_ = true;
    if (!members_) { trees(); return; }
    members_->initialize(); scores_->initialize();
    if (members_->error() != status::ok || scores_->error() != status::ok)
        tree_error();
}

bool zset_store::counts() {
    if (!members_ || !scores_) return fail(status::invalid);
    if (members_->metadata().elements != scores_->metadata().elements)
        return fail(status::invalid);
    root_.elements = members_->metadata().elements;
    return true;
}

bool zset_store::get(const std::string& member, zset_score& out) {
    out = zset_score();
    if (error_ != status::ok || !members_) return false;
    std::string bytes;
    bool found;
    if (!members_->get(member, bytes, found)) return tree_error();
    if (!found) return true;
    double score;
    if (!zset_page_codec::decode_value(bytes, score))
        return fail(status::invalid);
    std::string key;
    if (!zset_page_codec::score_key(score, member, key))
        return fail(status::invalid);
    uint64_t length;
    if (!scores_->length(key, length, found)) return tree_error();
    if (!found || length) return fail(status::invalid);
    out.found = true; out.score = score;
    return true;
}

bool zset_store::put(const std::string& member, double score, bool& added,
    bool& changed) {
    added = changed = false;
    if (error_ != status::ok || !members_) return false;
    if (std::isnan(score)) return fail(status::invalid);
    if (member.size() > map_page_codec::max_payload_bytes)
        return fail(status::too_large);
    zset_score old;
    if (!get(member, old)) return false;
    if (old.found && old.score == score) return true;
    if (!root_.elements && !entry_->clear_pages) {
        initialize(root_.expires_at_ms);
        if (error_ != status::ok) return false;
    }
    dirty_ = true;
    entry_->action = db::update_action::write;
    if (old.found) {
        std::string key;
        bool removed;
        if (!zset_page_codec::score_key(old.score, member, key))
            return fail(status::invalid);
        if (!scores_->erase(key, removed)) return tree_error();
        if (!removed) return fail(status::invalid);
    }
    std::string key, bytes;
    bool inserted;
    if (!zset_page_codec::score_key(score, member, key) ||
        !zset_page_codec::encode_value(score, bytes))
        return fail(status::invalid);
    if (!members_->put(member, bytes, false, added) ||
        !scores_->put(key, "", true, inserted)) {
        added = false; return tree_error();
    }
    if (!inserted || !counts()) { added = false; return fail(status::invalid); }
    changed = true;
    return true;
}

bool zset_store::erase(const std::string& member, bool& removed) {
    removed = false;
    if (error_ != status::ok || !members_) return false;
    zset_score old;
    if (!get(member, old)) return false;
    if (!old.found) return true;
    dirty_ = true;
    std::string key;
    bool deleted;
    if (!zset_page_codec::score_key(old.score, member, key))
        return fail(status::invalid);
    if (!scores_->erase(key, deleted) || !members_->erase(member, removed)) {
        removed = false; return tree_error();
    }
    if (!deleted || !removed || !counts()) {
        removed = false; return fail(status::invalid);
    }
    return true;
}

bool zset_store::set_expiry(int64_t expires_at_ms) {
    if (error_ != status::ok) return false;
    if (expires_at_ms < -1) return fail(status::invalid);
    if (root_.elements && root_.expires_at_ms != expires_at_ms) {
        root_.expires_at_ms = expires_at_ms; dirty_ = true;
    }
    return true;
}

bool zset_store::clear() {
    if (error_ != status::ok || !members_) return false;
    initialize();
    return error_ == status::ok;
}

bool zset_store::finish() {
    if (error_ != status::ok || !members_) return false;
    if (!members_->finish() || !scores_->finish()) return tree_error();
    if (!counts()) return false;
    if (!dirty_) return true;
    entry_->maintenance_only = false;
    root_.members = member_state_.tree; root_.scores = score_state_.tree;
    if (!root_.elements) {
        entry_->action = db::update_action::erase;
        entry_->clear_pages = false;
    } else {
        const auto result = zset_page_codec::encode_root(root_, entry_->value);
        if (result != map_page_status::ok) return fail(result ==
            map_page_status::out_of_memory ? status::storage : status::invalid);
        entry_->action = db::update_action::write;
    }
    dirty_ = false;
    return true;
}

} // namespace pkv
