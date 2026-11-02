#include "map_store.h"
#include <algorithm>
#include <climits>
#include <limits>

namespace pkv {

map_store::map_store(page_transaction& transaction, const std::string& owner,
    map_kind kind)
: transaction_(transaction), owner_(owner), kind_(kind)
, entry_(transaction.roots().find(owner)) {
    root_.kind = kind;
}

map_store::~map_store() {
    if (dirty_) transaction_.abort();
}

map_store::map_store(page_transaction& transaction, const std::string& owner,
    map_root& state, uint64_t& sequence, bool lexical)
: map_store(transaction, owner, state.kind) {
    embedded_ = &state; sequence_ = &sequence; lexical_ = lexical;
    root_ = state;
}

uint64_t map_store::last_id() const {
    return sequence_ ? *sequence_ : root_.last_id;
}

unsigned map_store::bucket(const std::string& key) const {
    return lexical_ ? 0 : static_cast<unsigned>(scan_bucket(key));
}

bool map_store::fail(status error) {
    if (error_ == status::ok) error_ = error;
    if (error != status::not_paged) transaction_.abort();
    return false;
}

bool map_store::decoded(map_page_status result) {
    switch (result) {
    case map_page_status::ok: return true;
    case map_page_status::unsupported: return fail(status::unsupported);
    case map_page_status::out_of_memory: return fail(status::storage);
    default: return fail(status::invalid);
    }
}

bool map_store::open() {
    if (!entry_) return fail(status::invalid);
    if (embedded_) {
        if (root_.kind != map_kind::hash || root_.expires_at_ms != -1 ||
            root_.elements > static_cast<uint64_t>(LLONG_MAX) ||
            root_.tree > last_id() || (root_.elements == 0) !=
                (root_.tree == 0)) return fail(status::invalid);
        return true;
    }
    if ((!entry_->found && entry_->action != db::update_action::write) ||
        entry_->action == db::update_action::erase) {
        root_ = map_root(); root_.kind = kind_;
        return true;
    }
    if (entry_->value.compare(0, 3, "PMR") != 0)
        return fail(status::not_paged);
    if (!decoded(map_page_codec::decode_root(entry_->value, root_)))
        return false;
    if (root_.kind != kind_) return fail(status::wrong_kind);
    return root_.elements != 0 || fail(status::invalid);
}

void map_store::initialize(int64_t expires_at_ms) {
    if (!entry_) { fail(status::invalid); return; }
    if (embedded_ && expires_at_ms != -1) { fail(status::invalid); return; }
    if (expires_at_ms < -1) { fail(status::invalid); return; }
    if (!embedded_ && !transaction_.reset_pages(owner_)) {
        fail(status::storage); return;
    }
    root_ = map_root(); root_.kind = kind_;
    root_.expires_at_ms = expires_at_ms;
    if (!embedded_) entry_->action = db::update_action::write;
    error_ = status::ok; dirty_ = true;
}

bool map_store::allocate(uint64_t& id) {
    if (last_id() == std::numeric_limits<uint64_t>::max())
        return fail(status::too_large);
    dirty_ = true;
    if (sequence_) id = ++*sequence_;
    else id = ++root_.last_id;
    return true;
}

bool map_store::read(uint64_t id, std::string& bytes) {
    if (!id || id > last_id()) return fail(status::invalid);
    const auto result = transaction_.lookup(owner_, id, bytes);
    if (result == db::lookup_status::found) return true;
    return fail(result == db::lookup_status::error ? status::storage :
        status::invalid);
}

bool map_store::load(uint64_t id, map_node& node) {
    std::string bytes;
    if (!read(id, bytes) ||
        !decoded(map_page_codec::decode_node(bytes, node))) return false;
    if (node.id != id || node.left > last_id() ||
        node.right > last_id() || node.key.head > last_id() ||
        node.value.head > last_id() || node.elements > root_.elements ||
        (!lexical_ && node.key.length > map_page_codec::max_payload_bytes) ||
        node.value.length > map_page_codec::max_payload_bytes ||
        (kind_ == map_kind::set && node.value.length != 0) ||
        (lexical_ && node.bucket != 0) || (!node.key.head &&
            bucket(node.key.inline_value) != node.bucket))
        return fail(status::invalid);
    return true;
}

bool map_store::write(const map_node& node) {
    std::string bytes;
    if (!decoded(map_page_codec::encode_node(node, bytes))) return false;
    return transaction_.put(owner_, node.id, bytes) || fail(status::storage);
}

bool map_store::stats(uint64_t id, uint32_t& height, uint64_t& count) {
    height = 0; count = 0;
    if (!id) return true;
    map_node node;
    if (!load(id, node)) return false;
    height = node.height; count = node.elements;
    return true;
}

bool map_store::refresh(map_node& node, int& balance) {
    uint32_t left, right;
    uint64_t before, after;
    if (!stats(node.left, left, before) || !stats(node.right, right, after))
        return false;
    if (left >= map_page_codec::max_height || right >=
        map_page_codec::max_height || before >= root_.elements ||
        after >= root_.elements || before > root_.elements - after - 1)
        return fail(status::invalid);
    node.height = std::max(left, right) + 1;
    node.elements = before + after + 1;
    balance = static_cast<int>(left) - static_cast<int>(right);
    return true;
}

bool map_store::check(const map_node& node) {
    map_node expected = node;
    int balance;
    if (!refresh(expected, balance)) return false;
    return (node.height == expected.height && node.elements ==
        expected.elements && balance >= -1 && balance <= 1) ||
        fail(status::invalid);
}

bool map_store::check_root() {
    if (!root_.elements) return root_.tree == 0 || fail(status::invalid);
    map_node node;
    if (!load(root_.tree, node) || !check(node)) return false;
    return node.elements == root_.elements || fail(status::invalid);
}

bool map_store::order(const map_node& node, const std::string& key,
    unsigned bucket, int& result) {
    if (node.bucket != bucket) {
        result = node.bucket < bucket ? -1 : 1;
        return true;
    }
    return compare(node.key, key, result);
}

bool map_store::find(const std::string& key, map_node& node, bool& found) {
    found = false;
    const auto bucket = this->bucket(key);
    uint64_t id = root_.tree;
    uint64_t limit = root_.elements;
    uint32_t height = map_page_codec::max_height + 1;
    while (id) {
        if (!load(id, node) || !check(node)) return false;
        if (node.elements > limit || node.height >= height)
            return fail(status::invalid);
        if (id == root_.tree && node.elements != root_.elements)
            return fail(status::invalid);
        int order;
        if (!this->order(node, key, bucket, order)) return false;
        if (!order) { found = true; return true; }
        limit = node.elements - 1; height = node.height;
        id = order > 0 ? node.left : node.right;
    }
    return true;
}

bool map_store::get(const std::string& key, std::string& value, bool& found) {
    bool exceeded;
    return get_bounded(key, UINT64_MAX, value, found, exceeded);
}

bool map_store::get_bounded(const std::string& key, uint64_t limit,
    std::string& value, bool& found, bool& exceeded) {
    exceeded = false;
    found = false;
    if (error_ != status::ok) return false;
    map_node node;
    bool present;
    if (!find(key, node, present)) return false;
    if (!present) return true;
    if (node.value.length > limit) {
        value.clear(); found = true; exceeded = true;
        return true;
    }
    if (!read_blob(node.value, value)) return false;
    found = true;
    return true;
}

bool map_store::length(const std::string& key, uint64_t& bytes, bool& found) {
    found = false; bytes = 0;
    if (error_ != status::ok) return false;
    map_node node;
    if (!find(key, node, found)) return false;
    if (found) bytes = node.value.length;
    return true;
}

bool map_store::put(const std::string& key, const std::string& value,
    bool only_missing, bool& added) {
    added = false;
    if (error_ != status::ok) return false;
    const auto limit = lexical_ ? map_page_codec::max_blob_bytes :
        map_page_codec::max_payload_bytes;
    if (key.size() > limit || value.size() >
        map_page_codec::max_payload_bytes) return fail(status::too_large);
    if (kind_ == map_kind::set && !value.empty()) return fail(status::invalid);
    if (!check_root()) return false;
    if (!root_.elements && !embedded_) {
        if (!transaction_.reset_pages(owner_)) return fail(status::storage);
        entry_->action = db::update_action::write;
    }
    if (!insert(root_.tree, key, value, only_missing, added,
        bucket(key), 0)) {
        added = false;
        return false;
    }
    return true;
}

bool map_store::erase(const std::string& key, bool& removed) {
    return erase_value(key, removed, nullptr);
}

bool map_store::take(const std::string& key, std::string& value, bool& found) {
    value.clear();
    if (erase_value(key, found, &value)) return true;
    value.clear(); found = false;
    return false;
}

bool map_store::erase_value(const std::string& key, bool& removed,
    std::string* value) {
    removed = false;
    if (error_ != status::ok) return false;
    if (!check_root()) return false;
    if (!remove(root_.tree, key, removed,
        bucket(key), 0, value)) {
        removed = false;
        return false;
    }
    if (removed && !root_.elements) return clear();
    return true;
}

bool map_store::set_expiry(int64_t expires_at_ms) {
    if (error_ != status::ok) return false;
    if (embedded_ && expires_at_ms != -1) return fail(status::invalid);
    if (expires_at_ms < -1) return fail(status::invalid);
    if (root_.elements && root_.expires_at_ms != expires_at_ms) {
        root_.expires_at_ms = expires_at_ms; dirty_ = true;
    }
    return true;
}

bool map_store::clear() {
    if (error_ != status::ok) return false;
    if (embedded_) {
        // erase has already reclaimed the final node and its blobs. Dropping
        // a live embedded tree would orphan pages in the shared family.
        if (root_.elements || root_.tree) return fail(status::invalid);
        dirty_ = true; return true;
    }
    if (!transaction_.reset_pages(owner_)) return fail(status::storage);
    root_.tree = root_.elements = 0;
    entry_->clear_pages = false;
    entry_->action = db::update_action::erase;
    dirty_ = true;
    return true;
}

bool map_store::finish() {
    if (error_ != status::ok) return false;
    if (embedded_) {
        root_.last_id = last_id(); *embedded_ = root_;
        dirty_ = false; return true;
    }
    if (!dirty_) return true;
    entry_->maintenance_only = false;
    if (!root_.elements) {
        entry_->action = db::update_action::erase;
        entry_->clear_pages = false;
        dirty_ = false;
        return true;
    }
    if (!decoded(map_page_codec::encode_root(root_, entry_->value)))
        return false;
    entry_->action = db::update_action::write;
    dirty_ = false;
    return true;
}

} // namespace pkv
