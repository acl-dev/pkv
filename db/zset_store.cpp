#include "coder/compact_zset.h"
#include "zset_store.h"
#include "page_node_allocator.h"
#include "coder/record_codec.h"
#include <cmath>
#include <cstring>

namespace pkv {
namespace {

class score_key_batch {
    std::vector<std::string> keys_;
    std::vector<std::string>& cache_;
public:
    explicit score_key_batch(size_t size) : cache_(local()) {
        // Swap the storage out so a nested call or another fiber gets its
        // own buffers while this request owns the live keys.
        keys_.swap(cache_);
        if (keys_.size() < size) keys_.resize(size);
    }
    score_key_batch(const score_key_batch&) = delete;
    score_key_batch& operator=(const score_key_batch&) = delete;
    ~score_key_batch() {
        if (!cache_.empty()) return;
        for (auto& key : keys_) {
            if (key.capacity() > map_page_codec::key_inline_bytes)
                std::string().swap(key);
            else key.clear();
        }
        keys_.swap(cache_);
    }
    std::string& operator[](size_t index) { return keys_[index]; }
private:
    static std::vector<std::string>& local() {
        static thread_local std::vector<std::string> cache;
        return cache;
    }
};

} // namespace

void zset_store::tree_deleter::operator()(map_store* value) const noexcept {
    if (!value) return;
    value->~map_store();
    detail::page_node_allocator<map_store>().deallocate(value, 1);
}

map_store* zset_store::create_tree(map_root& state, bool lexical) {
    detail::page_node_allocator<map_store> allocator;
    auto* storage = allocator.allocate(1);
    try {
        return ::new (static_cast<void*>(storage)) map_store(transaction_,
            entry_, state, root_.last_id, lexical);
    } catch (...) {
        // A failed constructor already destroyed its completed subobjects.
        allocator.deallocate(storage, 1);
        throw;
    }
}

zset_store::zset_store(page_transaction& transaction, const std::string& owner)
: transaction_(transaction), entry_(transaction.roots().find(owner)) {}

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
    try {
        members_.reset(create_tree(member_state_, false));
        scores_.reset(create_tree(score_state_, true));
    } catch (...) {
        scores_.reset(); members_.reset();
        fail(status::storage);
        throw;
    }
    use_crc_member_buckets(root_.crc_member_buckets);
    return (members_->open() && scores_->open()) || tree_error();
}

void zset_store::use_crc_member_buckets(bool enabled) {
    root_.crc_member_buckets = members_->crc_buckets_ = enabled;
}

bool zset_store::open() {
    return open_impl(expiry_mode::ignore, 0);
}

bool zset_store::open_live(long long now) {
    return open_impl(expiry_mode::erase, now);
}

bool zset_store::open_snapshot(long long now) {
    return open_impl(expiry_mode::hide, now);
}

bool zset_store::open_impl(expiry_mode expiry, long long now) {
    if (!entry_ || members_) return fail(status::invalid);
    if ((entry_->found || entry_->action == db::update_action::write) &&
        entry_->action != db::update_action::erase) {
        if (entry_->value.compare(0, 3, "PZR") != 0)
            return fail(status::not_paged);
        if (compact_zset::matches(entry_->value)) {
            try {
                const compact_zset::view packed(entry_->value);
                const auto deadline = packed.deadline();
                const auto values = packed.entries();
                if (expiry != expiry_mode::ignore && deadline >= 0 &&
                    deadline <= (now < 0 ? unix_time_ms() : now)) {
                    if (expiry == expiry_mode::erase)
                        entry_->action = db::update_action::erase;
                    return trees();
                }
                initialize(deadline);
                if (error_ != status::ok) return false;
                for (const auto& item : values) {
                    bool added, changed;
                    if (!put(item.first, item.second, added, changed) || !added)
                        return fail(status::invalid);
                }
                if (!finish()) return false;
                entry_->maintenance_only = true;
                return true;
            } catch (const std::runtime_error&) { return fail(status::invalid); }
        }
        const auto result = zset_page_codec::decode_root(entry_->value, root_);
        if (result != map_page_status::ok) return fail(result ==
            map_page_status::unsupported ? status::unsupported :
            status::invalid);
        if (!root_.elements) return fail(status::invalid);
        if (expiry != expiry_mode::ignore && root_.expires_at_ms >= 0 &&
            root_.expires_at_ms <= (now < 0 ? unix_time_ms() : now)) {
            if (expiry == expiry_mode::erase)
                entry_->action = db::update_action::erase;
            root_ = zset_root();
        }
    }
    return trees();
}

void zset_store::initialize(int64_t expires_at_ms) {
    if (!entry_ || expires_at_ms < -1) { fail(status::invalid); return; }
    if (!transaction_.reset_pages(entry_->key)) { fail(status::storage); return; }
    root_ = zset_root(); root_.expires_at_ms = expires_at_ms;
    entry_->action = db::update_action::write;
    error_ = status::ok; dirty_ = true;
    if (!members_) { trees(); return; }
    use_crc_member_buckets(false);
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

db::page_lock_scope* zset_store::repeated_read(const std::string& member,
    bool require_repeat) const {
    auto* scope = transaction_.scope_;
    // Only borrow an argument from the live pipeline. Opening a legacy or
    // expired key may stage changes even when the client issued a read.
    if (!scope || (require_repeat && !scope->cache_nodes_) ||
        scope->last_member_ != &member ||
        entry_->action != db::update_action::keep || entry_->clear_pages ||
        transaction_.failed_ || transaction_.has_changes()) return nullptr;
    return scope;
}

bool zset_store::get(const std::string& member, zset_score& out) {
    out = zset_score();
    if (error_ != status::ok || !members_) return false;
    auto* scope = repeated_read(member, false);
    if (scope && scope->scored_member_ && *scope->scored_member_ == member) {
        out.found = true;
        out.score = scope->member_score_;
        return true;
    }
    bool found;
    double score;
    if (!read_member_score(member, score, found)) return false;
    if (!found) return true;
    if (!validate_score(member, score, out)) return false;
    if (scope) {
        scope->scored_member_ = &member;
        scope->member_score_ = out.score;
        scope->member_rank_ = -1;
    }
    return true;
}

bool zset_store::read_member_score(const std::string& member,
    double& score, bool& found) {
    map_node node;
    if (members_->error() != status::ok || !members_->find(member, node, found))
        return tree_error();
    if (!found) return true;
    // A score has exactly eight bytes and always fits the inline payload.
    return (!node.value.head &&
        zset_page_codec::decode_value(node.value.inline_value, score)) ||
        fail(status::invalid);
}

bool zset_store::reuse_score_key(const std::string& member) {
    // Score-key scratch space can also be reused for a short fragmented key.
    return member.size() < detail::node_key_workspace::max_capacity - 8 &&
        member.size() + 8 > std::string().capacity();
}

bool zset_store::validate_score(const std::string& member, double score,
    zset_score& out) {
    detail::node_key_workspace key_buffer(reuse_score_key(member));
    auto& key = key_buffer.key.inline_value;
    if (!zset_page_codec::score_key(score, member, key))
        return fail(status::invalid);
    uint64_t length;
    bool found;
    if (!scores_->length(key, length, found)) return tree_error();
    if (!found || length) return fail(status::invalid);
    out.found = true; out.score = score;
    return true;
}

bool zset_store::get_scores_batched(const std::string* const* members,
    size_t count, zset_score* out) {
    if (error_ != status::ok || !members_ || !scores_) return false;
    constexpr size_t batch_size = 128;
    struct lookup_state {
        zset_store& store;
        zset_score* output;
    } state{*this, out};
    const auto member_found = [](void* raw, size_t index, const map_blob& value) {
        auto& job = *static_cast<lookup_state*>(raw);
        std::string bytes;
        if (value.head && !job.store.members_->read_blob(value, bytes))
            return job.store.tree_error();
        double score;
        if (!zset_page_codec::decode_value(value.head ? bytes : value.inline_value, score))
            return job.store.fail(status::invalid);
        job.output[index].found = true;
        job.output[index].score = score;
        return true;
    };
    const auto score_found = [](void* raw, size_t, const map_blob& value) {
        if (value.length) return false;
        --*static_cast<size_t*>(raw);
        return true;
    };
    std::fill_n(out, count, zset_score());
    map_store::lookup_request requests[batch_size];
    score_key_batch score_keys(std::min(count, batch_size));
    for (size_t start = 0; start < count;) {
        const auto size = std::min(batch_size, count - start);
        for (size_t i = 0; i < size; ++i)
            requests[i] = {members[start + i], start + i, 0};
        if (!members_->find_many(requests, requests + size, member_found, &state))
            return tree_error();
        size_t matches = 0;
        for (size_t i = start; i < start + size; ++i) {
            if (!out[i].found) continue;
            auto& key = score_keys[matches];
            if (!zset_page_codec::score_key(out[i].score, *members[i], key))
                return fail(status::invalid);
            requests[matches++] = {&key, i, 0};
        }
        auto remaining = matches;
        if (!scores_->find_many(requests, requests + matches,
                score_found, &remaining) || remaining)
            return tree_error();
        start += size;
    }
    return true;
}

bool zset_store::get_scores(const std::string* const* members, size_t count,
    zset_score* out) {
    auto* scope = count && count <= db::page_lock_scope::max_score_batch ?
        repeated_read(*members[0]) : nullptr;
    if (scope && scope->score_batch_size_ == count &&
        std::equal(members, members + count, scope->score_batch_members_,
            [](const std::string* a, const std::string* b) { return *a == *b; })) {
        std::copy_n(scope->score_batch_values_, count, out);
        return true;
    }
    constexpr size_t batch_size = 128;
    // Preserve the cheaper per-request cache when the first member
    // already repeats in the leading arguments.
    if (count >= batch_size && root_.elements && error_ == status::ok && members_ &&
        std::none_of(members + 1, members + 16,
            [&](const std::string* member) { return *member == *members[0]; })) {
        return get_scores_batched(members, count, out);
    }
    if (count >= batch_size && root_.elements) {
        // The request owns both the member strings and the result slots.
        // Collisions only cause another lookup, never a false match.
        size_t recent[64] = {};
        for (size_t i = 0; i < count; ++i) {
            const auto& member = *members[i];
            auto& slot = recent[std::hash<std::string>{}(member) % 64];
            if (slot && *members[slot - 1] == member)
                out[i] = out[slot - 1];
            else if (!get(member, out[i]))
                return false;
            slot = i + 1;
        }
        return true;
    }
    for (size_t i = 0; i < count; ++i) {
        if (!get(*members[i], out[i])) return false;
    }
    if (scope) {
        std::copy_n(members, count, scope->score_batch_members_);
        std::copy_n(out, count, scope->score_batch_values_);
        scope->score_batch_size_ = count;
    }
    return true;
}

bool zset_store::put(const std::string& member, double score, bool& added,
    bool& changed) {
    return put_score(member, score, false, added, changed);
}

bool zset_store::increment(const std::string& member, double& score,
    bool& added, bool& changed) {
    if (!std::isfinite(score)) return fail(status::invalid);
    return put_score(member, score, true, added, changed);
}

bool zset_store::put_score(const std::string& member, double& score,
    bool increment, bool& added, bool& changed) {
    added = changed = false;
    if (error_ != status::ok || !members_) return false;
    if (std::isnan(score)) return fail(status::invalid);
    if (member.size() > map_page_codec::max_payload_bytes)
        return fail(status::too_large);
    auto* repeated = increment ? nullptr : repeated_read(member, false);
    if (repeated && repeated->scored_member_ &&
        repeated->member_score_ == score && *repeated->scored_member_ == member)
        return true;
    size_t cached_index = 0;
    unsigned cached_bit = 0;
    if (repeated && repeated->zadd_score_mask_) {
        cached_index = std::hash<std::string>{}(member) %
            db::page_lock_scope::zadd_score_slots;
        cached_bit = 1U << cached_index;
        if (repeated->zadd_score_mask_ & cached_bit) {
            const auto& cached = repeated->zadd_scores_[cached_index];
            if (cached.score == score && *cached.member == member) return true;
        }
    }
    if (!root_.elements && !entry_->clear_pages) {
        initialize(root_.expires_at_ms);
        if (error_ != status::ok) return false;
    }
    if (member.size() >= map_store::crc_bucket_min && !root_.elements &&
        transaction_.database_.supports_compact_zsets())
        use_crc_member_buckets(true);
    std::string bytes;
    struct previous {
        zset_store& store;
        const std::string& member;
        double& score;
        bool increment;
        std::string& replacement;
        zset_score old;
    } state{*this, member, score, increment, bytes, zset_score()};
    auto inspect = [](void* raw, const map_blob& blob, bool& replace) {
        auto& value = *static_cast<previous*>(raw);
        std::string external;
        if (blob.head && !value.store.members_->read_blob(blob, external))
            return value.store.tree_error();
        const auto& bytes = blob.head ? external : blob.inline_value;
        double old;
        if (!zset_page_codec::decode_value(bytes, old))
            return value.store.fail(status::invalid);
        if (value.increment) value.score = old + value.score;
        replace = old != value.score;
        if (!replace)
            return value.store.validate_score(value.member, old, value.old);
        // The insertion traversal consumes the replacement after this check.
        if (value.increment &&
            !zset_page_codec::encode_value(value.score, value.replacement))
            return value.store.fail(status::invalid);
        // A changed score validates its old counterpart during deletion.
        // Both indexes remain staged in the same atomic page transaction.
        value.old.found = true; value.old.score = old;
        return true;
    };
    const map_store::value_gate gate{inspect, &state};
    if (!zset_page_codec::encode_value(score, bytes))
        return fail(status::invalid);
    auto* scope = increment ? nullptr : transaction_.scope_;
    bool handled = false;
    uint64_t shared_head = 0;
    const bool share_key = member.size() > map_page_codec::zset_key_inline_bytes &&
        transaction_.database_.supports_compact_zsets();
    // A recent hit favors the view traversal. A miss switches back to the
    // owning traversal so growing sets do not search twice per insertion.
    if (scope && scope->zadd_member_present_ && root_.elements >= 64 &&
        member.size() <= map_page_codec::zset_key_inline_bytes &&
        !members_->update_inline(member, bytes, gate, handled))
        return tree_error();
    if (!handled && !members_->put_checked(member, bytes, false, added, &gate,
        share_key ? &shared_head : nullptr))
        return tree_error();
    if (share_key && !shared_head) return fail(status::invalid);
    if (scope) scope->zadd_member_present_ = !added;
    const auto& old = state.old;
    if (!added && old.found && old.score == score) {
        if (repeated) {
            repeated->scored_member_ = &member;
            repeated->member_score_ = old.score;
            repeated->member_rank_ = -1;
            if (!cached_bit) {
                cached_index = std::hash<std::string>{}(member) %
                    db::page_lock_scope::zadd_score_slots;
                cached_bit = 1U << cached_index;
            }
            repeated->zadd_scores_[cached_index] = {&member, old.score};
            repeated->zadd_score_mask_ |= cached_bit;
        }
        return true;
    }
    dirty_ = true;
    entry_->action = db::update_action::write;
    const bool reuse_buffer = reuse_score_key(member);
    detail::node_key_workspace key_buffer(reuse_buffer);
    auto& key = key_buffer.key.inline_value;
    if (old.found) {
        bool removed;
        if (!zset_page_codec::score_key(old.score, member, key))
            return fail(status::invalid);
        auto empty = [](void* raw, const map_blob& blob, bool&) {
            return blob.length == 0 ||
                static_cast<map_store*>(raw)->fail(status::invalid);
        };
        const map_store::value_gate empty_value{empty, scores_.get()};
        // Deletion stages tombstones that insertion can consume. A short
        // external score key can reuse its fragment head as well.
        const bool reuse = root_.last_id <= UINT32_MAX &&
            key.size() <= map_page_codec::max_page_bytes -
                map_page_codec::fragment_header_bytes;
        if (reuse) {
            detail::node_key_workspace replacement_buffer(reuse_buffer);
            auto& replacement = replacement_buffer.key.inline_value;
            if (!zset_page_codec::score_key(score, member, replacement))
                return fail(status::invalid);
            if (!scores_->replace_score_key(key, replacement, shared_head) || !counts())
                return tree_error();
            changed = true;
            return true;
        }
        if (!scores_->erase_value(key, removed, nullptr, &empty_value)) {
            added = false; return tree_error();
        }
        if (!removed) { added = false; return fail(status::invalid); }
    }
    bool inserted;
    if (!zset_page_codec::score_key(score, member, key)) {
        added = false; return fail(status::invalid);
    }
    if (!scores_->put_checked(key, "", true, inserted, nullptr, nullptr, shared_head)) {
        added = false; return tree_error();
    }
    if (!inserted || !counts()) { added = false; return fail(status::invalid); }
    changed = true;
    return true;
}

bool zset_store::erase(const std::string& member, bool& removed) {
    removed = false;
    if (error_ != status::ok || !members_) return false;
    // A large removal rewrites shared ancestors repeatedly. Keep those
    // changes in the flat index as the two deletion paths accumulate.
    if (transaction_.flat_.size() >= page_transaction::initial_flat_limit)
        transaction_.wide_flat_enabled_ = true;
    std::string bytes;
    bool found;
    map_blob deferred_key;
    if (!(member.size() > map_page_codec::zset_key_inline_bytes ?
        members_->erase_value(member, found, &bytes, nullptr, 256, &deferred_key) :
        members_->take(member, bytes, found))) return tree_error();
    if (!found) return true;
    dirty_ = true;
    // Both deletes are staged. A broken score index aborts the transaction.
    double score;
    detail::node_key_workspace key_buffer(reuse_score_key(member));
    auto& key = key_buffer.key.inline_value;
    if (!zset_page_codec::decode_value(bytes, score) ||
        !zset_page_codec::score_key(score, member, key))
        return fail(status::invalid);
    auto empty = [](void* raw, const map_blob& blob, bool&) {
        return blob.length == 0 ||
            static_cast<map_store*>(raw)->fail(status::invalid);
    };
    const map_store::value_gate gate{empty, scores_.get()};
    bool deleted;
    if (!scores_->erase_value(key, deleted, nullptr, &gate))
        return tree_error();
    if (!deleted) return fail(status::invalid);
    // The score lookup may still need the member's fragment chain.
    if (!members_->erase_blob(deferred_key) || !counts()) return tree_error();
    removed = true;
    return true;
}

bool zset_store::pop_edge(bool maximum, zset_member& out) {
    if (error_ != status::ok || !members_ || !root_.elements)
        return fail(status::invalid);
    detail::node_key_workspace key_buffer(true, false);
    auto& key = key_buffer.key.inline_value;
    if (!scores_->take_edge(maximum, key)) return tree_error();
    return erase_edge_member(key, out, false);
}

bool zset_store::erase_edge_member(std::string& key, zset_member& out,
    bool batch, unsigned member_bucket) {
    dirty_ = true;
    zset_member value;
    double ordered_score;
    if (!zset_page_codec::decode_score_prefix(key, ordered_score))
        return fail(status::invalid);
    char saved_prefix[8];
    const char* prefix = key.data();
    const auto member_size = key.size() - sizeof(saved_prefix);
    const bool tight_wide_key = member_size > map_page_codec::zset_key_inline_bytes &&
        member_size <= 512 && key.capacity() - member_size <= 32;
    if (batch || tight_wide_key || (member_size <= map_page_codec::key_inline_bytes &&
        member_size > std::string().capacity())) {
        // Reuse the detached key when the reply would allocate a buffer.
        std::memcpy(saved_prefix, prefix, sizeof(saved_prefix));
        prefix = saved_prefix;
        key.erase(0, sizeof(saved_prefix));
        value.member.swap(key);
    } else {
        value.member.assign(key, sizeof(saved_prefix), std::string::npos);
    }
    std::string bytes;
    bool removed;
    if (!members_->erase_value(value.member, removed, &bytes, nullptr,
        member_bucket)) return tree_error();
    if (!removed || bytes.size() != 8) return fail(status::invalid);
    value.score = ordered_score;
    // The member index may retain -0, while score keys canonicalize zero.
    if (std::memcmp(prefix, bytes.data(), sizeof(saved_prefix)) != 0 &&
        (ordered_score != 0 || !zset_page_codec::decode_value(bytes, value.score) ||
            value.score != 0))
        return fail(status::invalid);
    if (!batch && !counts()) return false;
    out = std::move(value);
    return true;
}

bool zset_store::pop_edges(uint64_t count, bool maximum,
    std::vector<zset_member>& out) {
    // Large pops stage both index removals and the affected ancestor paths.
    if (count >= 16) transaction_.wide_flat_enabled_ = true;
    if (count >= 16 && count <= 512 && count < root_.elements &&
        error_ == status::ok && members_ && scores_) {
        std::vector<std::string> keys;
        if (!scores_->take_edges(static_cast<size_t>(count), maximum, keys))
            return tree_error();
        const auto compare_members = [](const std::string& a, const std::string& b) {
            return a.compare(std::min<size_t>(8, a.size()), std::string::npos,
                b, std::min<size_t>(8, b.size()), std::string::npos);
        };
        const bool descending = count >= 128 &&
            compare_members(keys.front(), keys.back()) > 0;
        if (count < 128 || std::is_sorted(keys.begin(), keys.end(),
            [&](const std::string& a, const std::string& b) {
                const auto order = compare_members(a, b);
                return descending ? order > 0 : order < 0;
            })) {
            for (auto& key : keys) {
                zset_member value;
                if (!erase_edge_member(key, value, true)) return false;
                out.push_back(std::move(value));
            }
            return counts();
        }
        // Visit nearby member-index paths together. Keep the score-index
        // positions so the reply still follows score order.
        constexpr uint16_t end = 512;
        std::array<uint16_t, 256> heads;
        std::array<uint16_t, end> next;
        heads.fill(end);
        // Prepend in reverse order to preserve the order within each bucket.
        for (auto i = static_cast<uint16_t>(keys.size()); i-- > 0;) {
            const auto& key = keys[i];
            if (key.size() < 8) return fail(status::invalid);
            auto& head = heads[members_->bucket(key.data() + 8, key.size() - 8)];
            next[i] = head;
            head = i;
        }
        const auto first = out.size();
        out.resize(first + keys.size());
        for (unsigned bucket = 0; bucket < heads.size(); ++bucket) {
            for (auto position = heads[bucket]; position != end; position = next[position]) {
                if (!erase_edge_member(keys[position], out[first + position], true, bucket)) {
                    out.resize(first);
                    return false;
                }
            }
        }
        return counts();
    }
    for (uint64_t i = 0; i < count; ++i) {
        zset_member value;
        if (!pop_edge(maximum, value)) return false;
        out.push_back(std::move(value));
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
