#include "coder/compact_hash.h"
#include "coder/compact_set.h"
#include "map_store.h"
#include "coder/record_codec.h"
#include "node_write_workspace.h"
#include <algorithm>
#include <climits>
#include <cstring>
#include <limits>

namespace pkv {

map_store::map_store(page_transaction& transaction, const std::string& owner,
    map_kind kind, bool adaptive)
: transaction_(transaction), owner_(owner), kind_(kind)
, entry_(transaction.roots().find(owner)), adaptive_(adaptive) {
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

map_store::map_store(page_transaction& transaction, db::batch_entry* entry,
    map_root& state, uint64_t& sequence, bool lexical)
: transaction_(transaction), kind_(state.kind), entry_(entry), root_(state)
, embedded_(&state), sequence_(&sequence), lexical_(lexical) {}

uint64_t map_store::last_id() const {
    return sequence_ ? *sequence_ : root_.last_id;
}

unsigned map_store::bucket(const std::string& key) const {
    return lexical_ ? 0 : static_cast<unsigned>(scan_bucket(key));
}

bool map_store::reuse_node_keys(const std::string& key) const {
    if (!embedded_ || root_.elements < 8 || key.size() <= std::string().capacity())
        return false;
    if (bound_bucket_ == 256)
        return key.size() <= map_page_codec::zset_key_inline_bytes;
    return key.size() <= map_page_codec::key_inline_bytes;
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
    return open_impl(false, 0);
}

bool map_store::open_live(long long now) {
    return open_impl(true, now);
}

bool map_store::open_impl(bool expire, long long now) {
    if (!entry_) return fail(status::invalid);
    if (embedded_) {
        if (root_.bucketed || (root_.kind != map_kind::hash &&
            root_.kind != map_kind::set) ||
            root_.expires_at_ms != -1 ||
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
    if (compact_set::matches(entry_->value)) {
        try {
            const compact_set::view packed(entry_->value);
            const auto deadline = packed.deadline();
            if (expire && deadline >= 0 &&
                deadline <= (now < 0 ? unix_time_ms() : now)) {
                entry_->action = db::update_action::erase;
                root_ = map_root(); root_.kind = kind_;
                return true;
            }
            if (kind_ != map_kind::set) return fail(status::wrong_kind);
            initialize(deadline);
            if (error_ != status::ok) return false;
            std::string members[compact_set::max_members];
            for (unsigned i = 0; i < packed.count(); ++i)
                members[i] = packed.at(i).str();
            long long added;
            const auto member_at = [&](size_t i) -> const std::string& {
                return members[i];
            };
            if (!build_set(member_at, packed.count(), added)) return false;
            if (added != packed.count()) return fail(status::invalid);
            if (!finish()) return false;
            entry_->maintenance_only = true;
            return true;
        } catch (const std::runtime_error&) { return fail(status::invalid); }
    }
    if (compact_hash::matches(entry_->value)) {
        try {
            const compact_hash::view packed(entry_->value);
            const auto deadline = packed.deadline();
            if (expire && deadline >= 0 &&
                deadline <= (now < 0 ? unix_time_ms() : now)) {
                entry_->action = db::update_action::erase;
                root_ = map_root(); root_.kind = kind_;
                return true;
            }
            if (kind_ != map_kind::hash) return fail(status::wrong_kind);
            initialize(deadline);
            if (error_ != status::ok) return false;
            std::pair<std::string, std::string> members[compact_hash::max_fields];
            hash_field_refs fields;
            fields.reserve(packed.count());
            for (unsigned i = 0; i < packed.count(); ++i) {
                const auto pair = packed.at(i);
                auto& member = members[i];
                member.first = pair.first.str();
                member.second = pair.second.str();
                fields.emplace_back(&member.first, &member.second);
            }
            int added;
            if (!build_hash(fields, added)) return false;
            if (added != static_cast<int>(packed.count()))
                return fail(status::invalid);
            if (!finish()) return false;
            entry_->maintenance_only = true;
            return true;
        } catch (const std::runtime_error&) { return fail(status::invalid); }
    }
    if (!decoded(map_page_codec::decode_root(entry_->value, root_)))
        return false;
    if (expire) {
        if (!root_.elements) return fail(status::invalid);
        if (root_.expires_at_ms >= 0 &&
            root_.expires_at_ms <= (now < 0 ? unix_time_ms() : now)) {
            entry_->action = db::update_action::erase;
            root_ = map_root(); root_.kind = kind_;
            return true;
        }
    }
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
    index_.reset();
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

bool map_store::read(uint64_t id, std::string& bytes, bool reuse) {
    if (!id || id > last_id()) return fail(status::invalid);
    const auto result = transaction_.lookup_bound(entry_, id, bytes, reuse);
    if (result == db::lookup_status::found) return true;
    return fail(result == db::lookup_status::error ? status::storage :
        status::invalid);
}

bool map_store::load(uint64_t id, map_node& node, bool with_value) {
    auto* scope = transaction_.scope_;
    map_node* cached = nullptr;
    if (with_value && scope && scope->cache_nodes_ &&
        embedded_ && id && id <= last_id() &&
        entry_ && !entry_->clear_pages &&
        entry_->action != db::update_action::erase &&
        !transaction_.failed_ && !transaction_.has_changes()) {
        if (!scope->nodes_) {
            scope->nodes_.reset(new (std::nothrow)
                map_node[db::page_lock_scope::node_cache_size]);
            if (!scope->nodes_) scope->cache_nodes_ = false;
        }
        if (scope->nodes_)
            cached = &scope->nodes_[id % db::page_lock_scope::node_cache_size];
        if (cached && cached->id == id) {
            if (!validate_node(id, *cached, cached->key.inline_value.data(),
                cached->key.inline_value.size())) return fail(status::invalid);
            node = *cached;
            return true;
        }
    }
    auto& bytes = node_bytes_.buffer();
    struct state { map_node& node; map_page_status result; bool with_value; } value{
        node, map_page_status::invalid, with_value};
    auto decode = [](void* raw, const std::string& encoded) {
        auto& job = *static_cast<state*>(raw);
        job.result = map_page_codec::decode_node_scratch(encoded, job.node,
            job.with_value);
    };
    if (!inspect(id, bytes, decode, &value) || !decoded(value.result))
        return false;
    if (!validate_node(id, node, node.key.inline_value.data(),
        node.key.inline_value.size())) return fail(status::invalid);
    if (cached) {
        // Publish the cache entry only after copying both owned payloads.
        try {
            map_node copy(node);
            *cached = std::move(copy);
        } catch (const std::bad_alloc&) {
            // The decoded node is still usable without caching it.
        }
    }
    return true;
}

template<class Node>
bool map_store::validate_node(uint64_t id, const Node& node, const char* key,
    size_t key_size) const {
    const auto limit = last_id();
    if ((bound_bucket_ < 256 && node.bucket != bound_bucket_) ||
        node.id != id || node.left > limit ||
        node.right > limit || node.key.head > limit ||
        node.value.head > limit || node.elements > root_.elements ||
        (!lexical_ && node.key.length > map_page_codec::max_payload_bytes) ||
        node.value.length > map_page_codec::max_payload_bytes ||
        (kind_ == map_kind::set && node.value.length != 0) ||
        (lexical_ ? node.bucket != 0 :
            (!node.key.head && !node.bucket_derived &&
                scan_bucket(key, key_size) != node.bucket)))
        return false;
    return true;
}

bool map_store::write(const map_node& node) {
    if (transaction_.wide_flat_enabled_) {
        if (auto* bytes = transaction_.staged_flat_write(node.id))
            return decoded(map_page_codec::encode_packed_node_scratch(node, *bytes, !lexical_));
    }
    detail::node_write_workspace workspace;
    auto& bytes = workspace.buffer();
    if (!decoded(map_page_codec::encode_packed_node_scratch(node, bytes, !lexical_)))
        return false;
    return transaction_.put_bound(entry_, node.id, std::move(bytes)) ||
        fail(status::storage);
}

bool map_store::load_child(uint64_t id, map_node& node,
    node_children& children) {
    auto* saved = id && id == children.left.id ? &children.left :
        id && id == children.right.id ? &children.right : nullptr;
    if (!saved) return load(id, node);
    // The saved slot becomes invalid. Transfer the decoded metadata once
    // and swap the strings so the slot can reuse the caller's buffers.
    node.id = saved->id;
    node.bucket = saved->bucket;
    node.bucket_derived = saved->bucket_derived;
    node.left = saved->left;
    node.right = saved->right;
    node.height = saved->height;
    node.elements = saved->elements;
    node.key.length = saved->key.length;
    node.key.head = saved->key.head;
    node.key.inline_value.swap(saved->key.inline_value);
    node.value.length = saved->value.length;
    node.value.head = saved->value.head;
    node.value.inline_value.swap(saved->value.inline_value);
    saved->id = 0;
    return true;
}

bool map_store::stats(uint64_t id, uint32_t& height, uint64_t& count,
    map_node* saved) {
    height = 0; count = 0;
    if (!id) {
        if (saved) saved->id = 0;
        return true;
    }
    if (saved) {
        if (!load(id, *saved)) return false;
        height = saved->height; count = saved->elements;
        return true;
    }
    struct state {
        map_store& store;
        uint64_t id;
        uint32_t height;
        uint64_t count;
        map_page_status result;
        bool valid;
    } value{*this, id, 0, 0, map_page_status::invalid, false};
    auto decode = [](void* raw, const std::string& bytes) {
        auto& job = *static_cast<state*>(raw);
        map_node_view node;
        job.result = map_page_codec::decode_node_view_scratch(bytes, node);
        if (job.result != map_page_status::ok) return;
        job.valid = job.store.validate_node(job.id, node, node.key.data,
            node.key.inline_size());
        if (job.valid) {
            job.height = node.height; job.count = node.elements;
        }
    };
    if (!inspect(id, node_bytes_.buffer(), decode, &value) ||
        !decoded(value.result)) return false;
    if (!value.valid) return fail(status::invalid);
    height = value.height; count = value.count;
    return true;
}

bool map_store::describe(const map_node& node, uint32_t& height,
    uint64_t& count, int& balance, node_children* children) {
    uint32_t left, right;
    uint64_t before, after;
    if (!stats(node.left, left, before, children ? &children->left : nullptr) ||
        !stats(node.right, right, after, children ? &children->right : nullptr))
        return false;
    return summarize(left, right, before, after, height, count, balance);
}

bool map_store::summarize(uint32_t left, uint32_t right, uint64_t before,
    uint64_t after, uint32_t& height, uint64_t& count, int& balance) {
    if (left >= map_page_codec::max_height || right >=
        map_page_codec::max_height || before >= root_.elements ||
        after >= root_.elements || before > root_.elements - after - 1)
        return fail(status::invalid);
    height = std::max(left, right) + 1;
    count = before + after + 1;
    balance = static_cast<int>(left) - static_cast<int>(right);
    return true;
}

bool map_store::check(const map_node& node, node_children* children) {
    uint32_t height;
    uint64_t count;
    int balance;
    if (!describe(node, height, count, balance, children)) return false;
    return (node.height == height && node.elements == count &&
        balance >= -1 && balance <= 1) ||
        fail(status::invalid);
}

bool map_store::check_root() {
    if (!root_.elements) return root_.tree == 0 || fail(status::invalid);
    map_node node;
    if (!load(root_.tree, node) || !check(node)) return false;
    return node.elements == root_.elements || fail(status::invalid);
}

bool map_store::check_path(const map_node& node, bool save_left,
    node_children& children, node_summary& left, node_summary& right) {
    (save_left ? children.right : children.left).id = 0;
    return check_path(node, save_left,
        save_left ? children.left : children.right, left, right);
}

bool map_store::check_path(const map_node& node, bool save_left,
    map_node& next, node_summary& left, node_summary& right) {
    // Only the next child needs an owning payload. Validate the sibling with
    // the same complete header decoder, retaining just its scalar summary.
    if (!stats(node.left, left.height, left.elements,
        save_left ? &next : nullptr) ||
        !stats(node.right, right.height, right.elements,
            save_left ? nullptr : &next)) return false;
    uint32_t height;
    uint64_t count;
    int balance;
    if (!summarize(left.height, right.height, left.elements, right.elements,
        height, count, balance)) return false;
    return (node.height == height && node.elements == count &&
        balance >= -1 && balance <= 1) || fail(status::invalid);
}

bool map_store::order(const map_node& node, const std::string& key,
    unsigned bucket, int& result) {
    if (node.bucket != bucket) {
        result = node.bucket < bucket ? -1 : 1;
        return true;
    }
#if defined(__GNUC__) && defined(__BYTE_ORDER__) && \
    __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    // Score index keys start with an order-preserving big-endian double.
    // Most comparisons can stop at that word without comparing the member.
    if (lexical_ && !node.key.head && node.key.inline_value.size() >= 8 &&
        key.size() >= 8) {
        uint64_t left, right;
        std::memcpy(&left, node.key.inline_value.data(), sizeof(left));
        std::memcpy(&right, key.data(), sizeof(right));
        if (left != right) {
            result = __builtin_bswap64(left) < __builtin_bswap64(right) ? -1 : 1;
            return true;
        }
    }
#endif
    return compare(node.key, key, result);
}

template<bool Ranked>
bool map_store::find_inline(const std::string& key, unsigned key_bucket,
    map_blob& result, bool& found, bool& fallback, map_node* match,
    const std::string* replacement, inline_edit* edit, uint64_t* rank) {
    struct probe {
        uint64_t left = 0, right = 0, count = 0;
        uint32_t height = 0;
        int order = 0;
        bool fragmented = false;
    };
    struct search {
        map_store& store;
        const std::string& key;
        map_blob& value;
        map_node* match;
        const std::string* replacement;
        inline_edit* edit;
        unsigned bucket;
        uint64_t id;
        probe node;
        map_page_status status;
    } job{*this, key, result, match, replacement, edit, key_bucket, 0, {},
        map_page_status::invalid};
    const auto decode = [](void* raw, const std::string& bytes) {
        auto& task = *static_cast<search*>(raw);
        map_node_view view;
        task.status = map_page_codec::decode_node_view_scratch(bytes, view);
        if (task.status != map_page_status::ok) return;
        if (!task.store.validate_node(task.id, view, view.key.data,
            view.key.inline_size())) {
            task.status = map_page_status::invalid;
            return;
        }
        auto& node = task.node;
        node.left = view.left; node.right = view.right;
        node.count = view.elements; node.height = view.height;
        node.fragmented = view.bucket == task.bucket && view.key.head;
        if (node.fragmented) return;
        if (view.bucket != task.bucket) {
            node.order = view.bucket < task.bucket ? -1 : 1;
            return;
        }
        const auto size = view.key.inline_size();
        const auto length = std::min(size, task.key.size());
        if (length == 3) {
            node.order = std::memcmp(view.key.data, task.key.data(), 3);
        } else if (task.store.lexical_ && length == 11) {
            // The score word precedes a three-byte member. Constant sizes
            // let the compiler compare both parts without a library call.
            node.order = std::memcmp(view.key.data, task.key.data(), 8);
            if (!node.order)
                node.order = std::memcmp(view.key.data + 8, task.key.data() + 8, 3);
        } else {
            node.order = std::memcmp(view.key.data, task.key.data(), length);
        }
        if (!node.order) node.order = (size > task.key.size()) -
            (size < task.key.size());
        if (!node.order) {
            if (task.match) {
                if (!task.replacement) {
                    task.status = map_page_codec::decode_node_scratch(bytes,
                        *task.match);
                    return;
                }
                auto& changed = *task.match;
                changed.value.head = view.value.head;
                // External values use the ordinary fragment replacement path.
                if (view.value.head) return;
                const auto& replacement = *task.replacement;
                changed.id = 0;
                if (view.value.length == replacement.size() &&
                    std::memcmp(view.value.data, replacement.data(),
                        replacement.size()) == 0) return;
                try {
                    changed.key.inline_value.assign(task.key);
                    changed.value.inline_value.assign(replacement);
                    changed.id = view.id;
                    changed.bucket = view.bucket;
                    changed.bucket_derived = view.bucket_derived;
                    changed.left = view.left;
                    changed.right = view.right;
                    changed.height = view.height;
                    changed.elements = view.elements;
                    changed.key.length = size;
                    changed.value.length = replacement.size();
                } catch (const std::bad_alloc&) {
                    task.status = map_page_status::out_of_memory;
                }
                return;
            }
            try {
                task.value.inline_value.assign(view.value.data,
                    view.value.inline_size());
                task.value.head = view.value.head;
                task.value.length = view.value.length;
                auto* edit = task.edit;
                if (edit && !view.value.head &&
                    view.value.length == edit->replacement.size()) {
                    edit->id = view.id;
                    if (task.value.inline_value != edit->replacement) {
                        // Preserve the validated page layout. The caller stages
                        // this copy only after checking the path and old score.
                        const auto offset = view.value.data - bytes.data();
                        edit->bytes.assign(bytes);
                        std::memcpy(&edit->bytes[offset], edit->replacement.data(),
                            edit->replacement.size());
                    }
                }
            } catch (const std::bad_alloc&) {
                task.status = map_page_status::out_of_memory;
            }
        }
    };
    auto read_probe = [&](uint64_t id, probe& node) {
        if (!id) { node = probe(); return true; }
        job.id = id;
        if (!inspect(id, node_bytes_.buffer(), decode, &job) ||
            !decoded(job.status)) return false;
        node = job.node;
        return true;
    };
    probe current;
    if (!read_probe(root_.tree, current)) return false;
    uint64_t limit = root_.elements;
    uint32_t height = map_page_codec::max_height + 1;
    bool root = true;
    uint64_t before = 0;
    while (current.count) {
        if (current.fragmented) { fallback = true; return true; }
        probe next;
        node_summary left, right;
        auto read_child = [&](uint64_t id, bool selected, node_summary& out) {
            if (!selected) return stats(id, out.height, out.elements);
            if (!read_probe(id, next)) return false;
            out = node_summary(next.height, next.count);
            return true;
        };
        if (!read_child(current.left, current.order > 0, left) ||
            !read_child(current.right, current.order < 0, right)) return false;
        uint32_t expected_height;
        uint64_t expected_count;
        int balance;
        if (!summarize(left.height, right.height, left.elements, right.elements,
            expected_height, expected_count, balance)) return false;
        if (current.height != expected_height || current.count != expected_count ||
            balance < -1 || balance > 1 || current.count > limit ||
            current.height >= height || (root && current.count != root_.elements))
            return fail(status::invalid);
        if (!current.order) {
            if (Ranked) *rank = before + left.elements;
            found = true; return true;
        }
        if (Ranked) {
            if (current.order < 0) before += left.elements + 1;
        }
        limit = current.count - 1;
        height = current.height;
        current = next;
        root = false;
    }
    return true;
}

bool map_store::rank_inline(const std::string& key, uint64_t& rank,
    bool& found, bool& fallback, bool require_empty) {
    map_blob value;
    bool present = false;
    uint64_t position = 0;
    if (!find_inline<true>(key, 0, value, present, fallback, nullptr,
        nullptr, nullptr, &position)) return false;
    if (fallback) return true;
    if (present && require_empty && value.length) return fail(status::invalid);
    rank = position; found = present;
    return true;
}

bool map_store::update_inline(const std::string& key, const std::string& value,
    const value_gate& gate, bool& handled) {
    map_blob old;
    detail::node_write_workspace workspace;
    auto& bytes = workspace.buffer();
    bytes.clear();
    inline_edit edit{value, bytes, 0};
    bool found = false, fallback = false;
    if (!find_inline(key, bucket(key), old, found, fallback, nullptr, nullptr, &edit))
        return false;
    if (!found || fallback || !edit.id) return true;
    handled = true;
    bool replace = true;
    if (!gate.inspect(gate.context, old, replace)) return false;
    if (!replace || bytes.empty()) return true;
    dirty_ = true;
    return transaction_.put_bound(entry_, edit.id, std::move(bytes)) ||
        fail(status::storage);
}

bool map_store::find(const std::string& key, map_node& node, bool& found) {
    found = false;
    const auto* scope = transaction_.scope_;
    const auto bucket = this->bucket(key);
    // ZSet member values contain only a score. The view traversal avoids
    // copying the intervening keys even in a large member index.
    constexpr uint64_t inline_search_limit = 16;
    const bool zset_members = embedded_ && !lexical_ && bound_bucket_ == 256;
    const auto key_limit = zset_members ? map_page_codec::zset_key_inline_bytes :
        map_page_codec::key_inline_bytes;
    if ((root_.elements <= inline_search_limit || zset_members) &&
        key.size() <= key_limit &&
        !(scope && scope->cache_nodes_)) {
        bool fallback = false;
        if (!find_inline(key, bucket, node.value, found, fallback)) return false;
        if (!fallback) return true;
    }
    uint64_t id = root_.tree;
    if (!id) return true;
    detail::node_key_workspace spare(reuse_node_keys(key));
    auto* current = &node;
    map_node* next = &spare;
    if (!load(id, *current)) return false;
    uint64_t limit = root_.elements;
    uint32_t height = map_page_codec::max_height + 1;
    while (id) {
        int order;
        if (!current->key.head || current->bucket != bucket) {
            if (!this->order(*current, key, bucket, order)) return false;
            node_summary left, right;
            if (order ? !check_path(*current, order > 0, *next, left, right) :
                !check(*current)) return false;
        } else {
            node_children children(reuse_node_keys(key));
            if (!check(*current, &children) ||
                !this->order(*current, key, bucket, order)) return false;
            if (order) {
                auto& child = order > 0 ? children.left : children.right;
                std::swap(*next, static_cast<map_node&>(child));
            }
        }
        if (current->elements > limit || current->height >= height)
            return fail(status::invalid);
        if (id == root_.tree && current->elements != root_.elements)
            return fail(status::invalid);
        if (!order) {
            if (current != &node) std::swap(node, *current);
            found = true;
            return true;
        }
        limit = current->elements - 1;
        height = current->height;
        id = order > 0 ? current->left : current->right;
        std::swap(current, next);
    }
    return true;
}

bool map_store::find_many(lookup_request* first, lookup_request* last,
    lookup_visitor visit, void* context) {
    for (auto* item = first; item != last; ++item)
        item->bucket = bucket(*item->key);
    std::sort(first, last, [](const lookup_request& a, const lookup_request& b) {
        return a.bucket == b.bucket ? *a.key < *b.key : a.bucket < b.bucket;
    });
    if (first == last) return true;
    map_node node;
    node_summary result;
    return find_many_path(root_.tree, root_.elements,
        map_page_codec::max_height + 1, first, last, visit, context, result, node);
}

namespace {
template<class Request, class Compare>
bool split_lookup_requests(Request*& first, Request* last, bool& matched,
    Compare compare) {
    while (first != last) {
        auto* pivot = first + (last - first) / 2;
        int comparison;
        if (!compare(*pivot, comparison)) return false;
        if (comparison > 0) first = pivot + 1;
        else {
            last = pivot;
            matched = comparison == 0;
        }
    }
    return true;
}
}

bool map_store::find_many_path(uint64_t id, uint64_t limit, uint32_t height,
    lookup_request* first, lookup_request* last,
    lookup_visitor visit, void* context, node_summary& result, map_node& node) {
    if (first == last || !id) return stats(id, result.height, result.elements);
    struct search {
        map_store& store;
        uint64_t id;
        map_node& node;
        lookup_request* middle;
        lookup_request* last;
        bool matched = false;
        map_page_status status = map_page_status::invalid;
        search(map_store& owner, uint64_t page, map_node& scratch,
            lookup_request* begin, lookup_request* end)
            : store(owner), id(page), node(scratch), middle(begin), last(end) {}
    } job(*this, id, node, first, last);
    const auto decode = [](void* raw, const std::string& bytes) {
        auto& task = *static_cast<search*>(raw);
        map_node_view view;
        task.status = map_page_codec::decode_node_view_scratch(bytes, view);
        if (task.status != map_page_status::ok) return;
        const auto size = view.key.inline_size();
        if (!task.store.validate_node(task.id, view, view.key.data, size)) {
            task.status = map_page_status::invalid;
            return;
        }
        auto& node = task.node;
        node.bucket = view.bucket;
        node.left = view.left;
        node.right = view.right;
        node.height = view.height;
        node.elements = view.elements;
        node.key.head = view.key.head;
        node.key.length = view.key.length;
        if (!view.key.head) {
            split_lookup_requests(task.middle, task.last, task.matched,
                [&](const lookup_request& item, int& comparison) {
                    if (view.bucket != item.bucket) {
                        comparison = view.bucket < item.bucket ? -1 : 1;
                        return true;
                    }
                    const auto length = std::min(size, item.key->size());
                    comparison = std::memcmp(view.key.data, item.key->data(), length);
                    if (!comparison) comparison = (size > item.key->size()) -
                        (size < item.key->size());
                    return true;
                });
        }
        node.value.head = view.value.head;
        node.value.length = view.value.length;
        // The visitor runs after inspection. Own only the matched value,
        // or a possible match when the key comparison needs fragments.
        if (!task.store.lexical_ && (task.matched || view.key.head)) {
            try {
                node.value.inline_value.assign(view.value.data, view.value.inline_size());
            } catch (const std::bad_alloc&) {
                task.status = map_page_status::out_of_memory;
            }
        }
    };
    if (!inspect(id, node_bytes_.buffer(), decode, &job) || !decoded(job.status))
        return false;
    if (node.elements > limit || node.height >= height ||
        (id == root_.tree && node.elements != root_.elements))
        return fail(status::invalid);

    // The sorted requests for each child form a contiguous subrange.
    auto* middle = job.middle;
    bool matched = job.matched;
    if (node.key.head && !split_lookup_requests(middle, last, matched,
            [&](const lookup_request& item, int& comparison) {
                return order(node, *item.key, item.bucket, comparison);
            })) return false;
    auto* right = middle;
    if (matched) {
        // The last update to the upper bound compared the first candidate.
        // Equal requests reuse that match, including fragmented keys.
        do {
            if (!visit(context, right->index, node.value)) return false;
            ++right;
        } while (right != last && *right->key == *middle->key);
    }
    // Both children reuse the same decoded node storage. Keep only the
    // current node's links and expected summary across those calls.
    const auto left = node.left;
    const auto next = node.right;
    const node_summary expected(node.height, node.elements);
    // A visited child already decoded its header. Only an unvisited child
    // needs a separate summary read to validate the parent's metadata.
    node_summary left_summary, right_summary;
    if (!find_many_path(left, expected.elements - 1, expected.height,
            first, middle, visit, context, left_summary, node) ||
        !find_many_path(next, expected.elements - 1, expected.height,
            right, last, visit, context, right_summary, node)) return false;
    int balance;
    if (!summarize(left_summary.height, right_summary.height,
            left_summary.elements, right_summary.elements,
            result.height, result.elements, balance)) return false;
    if (result.height != expected.height || result.elements != expected.elements ||
        balance < -1 || balance > 1) return fail(status::invalid);
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
    if (root_.bucketed) {
        auto apply = [&](map_store& child) {
            return child.get_bounded(key, limit, value, found, exceeded);
        };
        return bucket_run(bucket(key), std::ref(apply));
    }
    detail::node_key_workspace node(reuse_node_keys(key));
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

bool map_store::probe_hash_field(const std::string& field, std::string& value,
    bool& found) {
    found = false;
    if (error_ != status::ok) return false;
    if (kind_ != map_kind::hash) return fail(status::wrong_kind);
    if (!root_.bucketed) return get(field, value, found);
    if (!probe_directory_ready_) {
        if (!read_directory(false)) return false;
        probe_directory_ready_ = true;
        if (root_.grouped) index_->directory.reset(new map_directory);
    }
    const auto index = bucket(field);
    if (root_.grouped) {
        const auto group = index / 16;
        const unsigned bit = 1U << group;
        if (!(probe_groups_ & bit)) {
            if (!read_group(group)) return false;
            std::copy(index_->group.buckets.begin(), index_->group.buckets.end(),
                index_->directory->buckets.begin() + group * 16);
            probe_groups_ |= bit;
        }
    }
    const auto& entry = index_->directory->buckets[index];
    map_root state;
    state.kind = kind_;
    state.tree = entry.tree;
    state.elements = entry.elements;
    state.last_id = root_.last_id;
    map_store child(transaction_, entry_, state, root_.last_id, false);
    child.bound_bucket_ = index;
    if (child.open() && child.get(field, value, found)) return true;
    return fail(child.error_);
}

bool map_store::length(const std::string& key, uint64_t& bytes, bool& found) {
    found = false; bytes = 0;
    if (error_ != status::ok) return false;
    if (root_.bucketed) {
        auto apply = [&](map_store& child) {
            return child.length(key, bytes, found);
        };
        return bucket_run(bucket(key), std::ref(apply));
    }
    const auto* scope = transaction_.scope_;
    if (lexical_ && root_.elements > 16 &&
        key.size() <= map_page_codec::key_inline_bytes &&
        !(scope && scope->cache_nodes_)) {
        map_blob value;
        bool fallback = false;
        if (!find_inline(key, 0, value, found, fallback)) return false;
        if (!fallback) {
            if (found) bytes = value.length;
            return true;
        }
    }
    detail::node_key_workspace node(reuse_node_keys(key));
    if (!find(key, node, found)) return false;
    if (found) bytes = node.value.length;
    return true;
}

bool map_store::hash_field(const std::string& field, std::string* value,
    uint64_t& bytes, bool& found) {
    auto* scope = transaction_.scope_;
    if (scope && (field.size() > 1024 || !entry_ ||
        entry_->action != db::update_action::keep || entry_->clear_pages ||
        transaction_.failed_ || transaction_.has_changes())) scope = nullptr;
    if (scope && scope->hash_valid_ && scope->hash_field_ == field &&
        (!value || !scope->hash_found_ || scope->hash_has_value_)) {
        found = scope->hash_found_;
        bytes = scope->hash_length_;
        if (value && found) *value = scope->hash_value_;
        return true;
    }
    if (value) {
        if (!get(field, *value, found)) return false;
        bytes = found ? value->size() : 0;
    } else if (!length(field, bytes, found)) return false;
    if (scope) {
        scope->hash_valid_ = false;
        try {
            scope->hash_field_ = field;
            scope->hash_has_value_ = value && found && bytes <= 1024;
            if (scope->hash_has_value_) scope->hash_value_ = *value;
            scope->hash_length_ = bytes;
            scope->hash_found_ = found;
            scope->hash_valid_ = true;
        } catch (const std::bad_alloc&) {
            // A cache allocation failure does not discard the lookup result.
        }
    }
    return true;
}

bool map_store::hash_fields(const std::string* const* fields, size_t count,
    std::string* values, unsigned& found) {
    found = 0;
    if (kind_ != map_kind::hash || count > db::page_lock_scope::max_hash_batch)
        return fail(status::invalid);
    auto* scope = transaction_.scope_;
    if (scope && (!entry_ || entry_->action != db::update_action::keep ||
        entry_->clear_pages || transaction_.failed_ ||
        transaction_.has_changes())) scope = nullptr;
    if (scope && count && scope->hash_batch_size_ == count &&
        std::equal(fields, fields + count, scope->hash_batch_fields_,
            [](const std::string* a, const std::string* b) { return *a == *b; })) {
        std::copy_n(scope->hash_batch_values_, count, values);
        found = scope->hash_batch_found_;
        return true;
    }
    size_t remaining = 4096;
    for (size_t i = 0; i < count; ++i) {
        bool present;
        if (!get(*fields[i], values[i], present)) return false;
        if (present) {
            found |= 1U << i;
            if (values[i].size() > remaining) scope = nullptr;
            else remaining -= values[i].size();
        }
    }
    if (scope && count) {
        scope->hash_batch_size_ = 0;
        try {
            std::copy_n(values, count, scope->hash_batch_values_);
            std::copy_n(fields, count, scope->hash_batch_fields_);
            scope->hash_batch_found_ = found;
            scope->hash_batch_size_ = count;
        } catch (const std::bad_alloc&) {
            // The result is still usable without caching it.
        }
    }
    return true;
}

bool map_store::set_membership(const std::string* const* members, size_t count,
    unsigned& found) {
    found = 0;
    if (kind_ != map_kind::set || count > db::page_lock_scope::max_membership_batch)
        return fail(status::invalid);
    auto* scope = transaction_.scope_;
    if (scope && (entry_->action != db::update_action::keep ||
        entry_->clear_pages || transaction_.failed_ ||
        transaction_.has_changes())) scope = nullptr;
    if (scope && count && scope->membership_size_ == count &&
        std::equal(members, members + count, scope->membership_args_,
            [](const std::string* a, const std::string* b) { return *a == *b; })) {
        found = scope->membership_mask_;
        return true;
    }
    for (size_t i = 0; i < count; ++i) {
        uint64_t bytes;
        bool present;
        if (!length(*members[i], bytes, present)) return false;
        if (present) found |= 1U << i;
    }
    if (scope && count) {
        std::copy_n(members, count, scope->membership_args_);
        scope->membership_mask_ = found;
        scope->membership_size_ = count;
    }
    return true;
}

bool map_store::probe_set_member(const std::string& member, bool& found) {
    if (kind_ != map_kind::set) return fail(status::wrong_kind);
    uint64_t bytes;
    if (!root_.bucketed) return length(member, bytes, found);
    if (!probe_directory_ready_) {
        if (!read_directory(false)) return false;
        probe_directory_ready_ = true;
    }
    auto probe = [&](map_store& child) {
        return child.length(member, bytes, found);
    };
    return bucket_run(bucket(member), std::ref(probe), false);
}

bool map_store::put(const std::string& key, const std::string& value,
    bool only_missing, bool& added) {
    return put_checked(key, value, only_missing, added, nullptr);
}

bool map_store::transform_hash_field(const std::string& key,
    uint64_t old_limit, const hash_transform& fn) {
    if (kind_ != map_kind::hash) return fail(status::invalid);
    struct state {
        map_store& store;
        uint64_t old_limit;
        const hash_transform& fn;
    } job{*this, old_limit, fn};
    const auto transform = [](void* raw, const map_blob* old, std::string& out) {
        auto& job = *static_cast<state*>(raw);
        std::string value;
        const bool exceeded = old && old->length > job.old_limit;
        if (old && !exceeded && !job.store.read_blob(*old, value)) return false;
        if (!job.fn(old ? &value : nullptr, exceeded, out)) return false;
        return out.size() <= map_page_codec::max_payload_bytes ||
            job.store.fail(status::too_large);
    };
    const value_gate gate{nullptr, &job, transform};
    bool added;
    return put_checked(key, "", false, added, &gate);
}

bool map_store::put_checked(const std::string& key, const std::string& value,
    bool only_missing, bool& added, const value_gate* gate) {
    added = false;
    if (error_ != status::ok) return false;
    const auto limit = lexical_ ? map_page_codec::max_blob_bytes :
        map_page_codec::max_payload_bytes;
    if (key.size() > limit || value.size() >
        map_page_codec::max_payload_bytes) return fail(status::too_large);
    if (kind_ == map_kind::set && !value.empty()) return fail(status::invalid);
    const auto key_bucket = bucket(key);
    if (root_.bucketed) {
        auto apply = [&](map_store& child) {
            return child.put_in_tree(key, value, key_bucket, only_missing,
                added, gate);
        };
        const bool ok = bucket_run(key_bucket, std::ref(apply));
        if (!ok) added = false;
        return ok;
    }
    return put_in_tree(key, value, key_bucket, only_missing, added, gate);
}

bool map_store::put_in_tree(const std::string& key, const std::string& value,
    unsigned key_bucket, bool only_missing, bool& added, const value_gate* gate) {
    if ((root_.elements == 0) != (root_.tree == 0))
        return fail(status::invalid);
    auto* scope = transaction_.scope_;
    if (scope && !gate && !only_missing && kind_ == map_kind::hash &&
        bound_bucket_ < 256) {
        const bool probe = scope->hash_inline_hit_ && root_.elements >= 8 &&
            key.size() <= map_page_codec::key_inline_bytes &&
            value.size() <= map_page_codec::value_inline_bytes;
        scope->hash_inline_hit_ = false;
        if (probe) {
            map_node node;
            bool found = false, fallback = false;
            if (!find_inline(key, key_bucket, node.value, found, fallback,
                &node, &value)) return false;
            if (found && !fallback && !node.value.head) {
                dirty_ = true;
                scope->hash_inline_hit_ = true;
                return !node.id || write(node);
            }
        }
    }
    auto* set_scope = only_missing && kind_ == map_kind::set &&
        bound_bucket_ < 256 ? scope : nullptr;
    if (set_scope && root_.elements >= 4 &&
        (root_.elements <= 8 || set_scope->set_member_present_) &&
        key.size() <= map_page_codec::key_inline_bytes) {
        // A repeated SADD needs the same tree validation but no owning nodes.
        map_blob unused;
        bool found = false, fallback = false;
        if (!find_inline(key, key_bucket, unused, found, fallback)) return false;
        if (found && !fallback) {
            set_scope->set_member_present_ = true;
            return true;
        }
    }
    if (!root_.elements && !embedded_) {
        if (!transaction_.reset_pages(owner_)) return fail(status::storage);
        entry_->action = db::update_action::write;
    }
    node_children children(reuse_node_keys(key));
    node_summary updated;
    if (!insert(root_.tree, key, value, only_missing, added,
        key_bucket, 0, children, updated, gate)) {
        added = false;
        return fail(error_ == status::ok ? status::invalid : error_);
    }
    if (set_scope) set_scope->set_member_present_ = !added;
    const uint64_t partition_size = kind_ == map_kind::hash ?
        hash_partition_size : set_partition_size;
    if (adaptive_ && !embedded_ && added &&
        root_.last_id <= std::numeric_limits<uint64_t>::max() - 17 &&
        root_.elements == partition_size && !partition()) {
        added = false;
        return false;
    }
    return true;
}

bool map_store::erase(const std::string& key, bool& removed) {
    return erase_value(key, removed, nullptr);
}

bool map_store::insert_grouped_set(const set_member_at& member_at, size_t count,
    long long& added) {
    added = 0;
    if (error_ != status::ok || kind_ != map_kind::set || !root_.grouped)
        return fail(status::invalid);
    if (!read_directory(false)) return false;
    // Keep ancestor rewrites in the flat index across the member batch.
    transaction_.wide_flat_enabled_ = true;
    set_pop_groups deferred;
    deferred.pages.reset(new (std::nothrow) std::array<map_group, 16>);
    const std::string empty;
    // Keep temporary storage bounded even for very large commands. Sorting
    // by bucket and request offset preserves insertion order within each tree.
    std::array<std::pair<unsigned, size_t>, 128> ordered;
    for (size_t base = 0; base < count;) {
        const auto length = std::min(ordered.size(), count - base);
        for (size_t i = 0; i < length; ++i) {
            const auto& member = member_at(base + i);
            if (member.size() > map_page_codec::max_payload_bytes)
                return fail(status::too_large);
            ordered[i] = {bucket(member), i};
        }
        std::sort(ordered.begin(), ordered.begin() + length);
        for (size_t i = 0; i < length;) {
            const auto index = ordered[i].first;
            auto apply = [&](map_store& child) {
                do {
                    const auto& member = member_at(base + ordered[i].second);
                    bool inserted = false;
                    if (!child.put_in_tree(member, empty, index, true,
                            inserted, nullptr)) return false;
                    if (inserted) ++added;
                    ++i;
                } while (i < length && ordered[i].first == index);
                return true;
            };
            if (!bucket_run(index, std::ref(apply), false, &deferred))
                return false;
        }
        base += length;
    }
    return !added || finish_set_pop(deferred);
}

bool map_store::erase_set(const set_member_at& member_at, size_t count,
    long long& removed) {
    removed = 0;
    if (error_ != status::ok || kind_ != map_kind::set)
        return fail(status::invalid);
    const bool grouped = root_.grouped && count > 1;
    if (grouped && !read_directory(false)) return false;
    set_pop_groups deferred;
    if (grouped && count >= 32)
        deferred.pages.reset(new (std::nothrow) std::array<map_group, 16>);
    for (size_t i = 0; i < count && root_.elements; ++i) {
        const auto& member = member_at(i);
        bool found;
        if (grouped) {
            auto apply = [&](map_store& child) { return child.erase(member, found); };
            if (!bucket_run(bucket(member), std::ref(apply), false, &deferred))
                return false;
        } else if (!erase(member, found)) return false;
        if (found) ++removed;
    }
    return !grouped || !removed || !root_.elements || finish_set_pop(deferred);
}

bool map_store::take(const std::string& key, std::string& value, bool& found) {
    value.clear();
    if (erase_value(key, found, &value)) return true;
    value.clear(); found = false;
    return false;
}

bool map_store::erase_value(const std::string& key, bool& removed,
    std::string* value, const value_gate* gate, unsigned key_bucket) {
    removed = false;
    if (error_ != status::ok) return false;
    if (root_.bucketed) {
        auto apply = [&](map_store& child) {
            return child.erase_value(key, removed, value, gate);
        };
        const bool ok = bucket_run(bucket(key), std::ref(apply));
        if (!ok) removed = false;
        return ok;
    }
    if ((root_.elements == 0) != (root_.tree == 0))
        return fail(status::invalid);
    node_children children(reuse_node_keys(key));
    node_summary updated;
    if (!remove(root_.tree, key, removed,
        key_bucket == 256 ? bucket(key) : key_bucket, 0, children, updated,
        value, gate)) {
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
    root_.bucketed = false; root_.grouped = false;
    index_.reset();
    entry_->clear_pages = false;
    entry_->action = db::update_action::erase;
    dirty_ = true;
    return true;
}

bool map_store::collect_packed_set(std::vector<std::string>& members,
    std::unordered_set<uint64_t>& seen, size_t& payload, bool& eligible) {
    if (root_.bucketed) {
        if (!read_directory()) return false;
        auto collect = [&](map_store& child) {
            return child.collect_packed_set(members, seen, payload, eligible);
        };
        for (unsigned i = 0; i < 256 && eligible; ++i) {
            if (index_->directory->buckets[i].elements &&
                !bucket_run(i, std::ref(collect), false)) return false;
        }
        return true;
    }
    const auto first = members.size();
    cursor reader(*this);
    map_node node;
    bool present;
    while (reader.next(node, present)) {
        if (!present)
            return members.size() - first == root_.elements || fail(status::invalid);
        if (node.key.length > 255 ||
            node.key.length + 1 > compact_set::payload_limit - payload) {
            eligible = false;
            return true;
        }
        if (members.size() == compact_set::max_members ||
            node.value.length || !seen.insert(node.id).second)
            return fail(status::invalid);
        std::string key;
        if (node.key.head) {
            key.reserve(static_cast<size_t>(node.key.length));
            if (!visit_blob(node.key, [&](const map_fragment_view& page) {
                if (!seen.insert(page.id).second) return fail(status::invalid);
                key.append(page.data, page.size);
                return true;
            })) return false;
        } else key = node.key.inline_value;
        if (bucket(key) != node.bucket) return fail(status::invalid);
        if (!members.empty()) {
            const auto previous_bucket = bucket(members.back());
            if (previous_bucket > node.bucket ||
                (previous_bucket == node.bucket && members.back() >= key))
                return fail(status::invalid);
        }
        payload += key.size() + 1;
        members.push_back(std::move(key));
    }
    return false;
}

bool map_store::pack_set_root(bool& packed) {
    packed = false;
    std::vector<std::string> members;
    members.reserve(static_cast<size_t>(root_.elements));
    std::unordered_set<uint64_t> seen;
    size_t payload = 0;
    bool eligible = true;
    if (!collect_packed_set(members, seen, payload, eligible)) return false;
    if (!eligible) return true;
    if (members.size() != root_.elements) return fail(status::invalid);
    auto bytes = compact_set::encode(members, root_.expires_at_ms);
    if (bytes.empty()) return fail(status::invalid);
    // Finish all validation/allocation before dropping the staged family.
    if (!transaction_.reset_pages(owner_)) return fail(status::storage);
    entry_->value = std::move(bytes);
    entry_->action = db::update_action::write;
    dirty_ = false;
    packed = true;
    return true;
}

bool map_store::finish(bool pack_set) {
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
    if (pack_set && kind_ == map_kind::set &&
        root_.elements <= compact_set::max_members) {
        bool packed;
        if (!pack_set_root(packed)) return false;
        if (packed) return true;
    }
    if (!decoded(map_page_codec::encode_root(root_, entry_->value)))
        return false;
    entry_->action = db::update_action::write;
    dirty_ = false;
    return true;
}

} // namespace pkv
