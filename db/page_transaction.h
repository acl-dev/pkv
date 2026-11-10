#pragma once
#include "db.h"
#include "page_node_allocator.h"
#include <map>
#include <utility>

namespace pkv {

struct page_change {
    std::string owner;
    uint64_t id;
    std::string value;
    db::update_action action;

    template<class Key, class Bytes>
    page_change(Key&& key, uint64_t number, Bytes&& bytes,
        db::update_action operation)
    : owner(std::forward<Key>(key))
    , id(number)
    , value(std::forward<Bytes>(bytes))
    , action(operation)
    {}
};

// Borrowed immutable input to ordered replay. Owners must retain these vectors
// until replica_apply_pages returns; none may be concurrently modified.
struct page_update {
    const std::vector<db::batch_entry>& roots;
    const std::vector<page_change>& pages;
    // Local workspace snapshots; replay defaults to receiver-side waiters.
    unsigned ready_types = ~0U;
    page_update(const std::vector<db::batch_entry>& metadata,
        const std::vector<page_change>& changes) : roots(metadata),
        pages(changes) {}
};

// A page address is (logical key, nonzero page id) in a separate native
// namespace. No prefix in the user-key byte space is reserved. This view and
// its roots cannot outlive the callback or be used from another thread.
class page_transaction {
public:
    static const size_t max_value_bytes = db::page_inspection::max_bytes;
    db::batch_view roots() const { return roots_; }

    // Reads observe staged writes/deletes. Missing/error leave out unchanged.
    // A rejected address/write poisons the transaction, even if ignored by
    // the callback. Lvalue inputs are copied; rvalues transfer ownership.
    // Returned data owns its bytes.
    db::lookup_status lookup(const std::string& owner, uint64_t id,
        std::string& out);
    // Reuse a caller's working buffer. Missing/error may change its contents;
    // successful reads still own their bytes. Storage failures poison the tx.
    db::lookup_status lookup_scratch(const std::string& owner, uint64_t id,
        std::string& scratch);
    bool put(const std::string& owner, uint64_t id, const std::string& bytes);
    bool put(const std::string& owner, uint64_t id, std::string&& bytes);
    bool erase(const std::string& owner, uint64_t id);
    // Discard prior staged pages and clear the native family at commit.
    // Caller must explicitly stage a root write after selecting new metadata.
    bool reset_pages(const std::string& owner);
    // Seal a complete replacement: require staged writes at IDs 1..last,
    // discard staged changes beyond last and drop the old family at commit.
    // Keeps old pages readable until the producer has staged every new page.
    bool replace_pages(const std::string& owner, uint64_t last);
    void abort() { failed_ = true; }

private:
    friend class db;
    friend class map_store;
    friend class packed_hash_store;
    friend class zset_store;
    friend class list_store;
    friend class string_store;
    friend class stream_store;
    page_transaction(db& database, db::batch_view roots,
        std::vector<page_change>* page_buffer = nullptr,
        db::page_lock_scope* scope = nullptr, std::string* page_owner_buffer = nullptr)
    : database_(database), roots_(roots),
      flat_(page_buffer ? *page_buffer : owned_flat_), scope_(scope),
      page_owner_buffer_(page_owner_buffer) {}

    // Roots are immutable, contiguous and sorted by key for this scope.
    // Their ordinal preserves owner ordering without copying/comparing names.
    using address = std::pair<size_t, uint64_t>;
    size_t ordinal(const db::batch_entry* root) const {
        return static_cast<size_t>(root - roots_.begin());
    }
    db& database_;
    db::batch_view roots_;
    page_change first_{"", 0, "", db::update_action::keep};
    enum : size_t { initial_flat_limit = 40, flat_limit = 120,
        wide_reserve_limit = 768, wide_flat_limit = 2048,
        flat_index_size = 256, wide_flat_index_size = 4096 };
    // Small single-owner writes can append without allocating tree nodes.
    std::vector<page_change> owned_flat_;
    std::vector<page_change>& flat_;
    // One-based positions in the append-only, unsorted single-owner vector.
    // The load is bounded by flat_limit; zero ends a probe.
    static_assert(flat_limit < flat_index_size, "flat index needs an empty slot");
    static_assert(flat_limit <= UINT8_MAX, "flat positions fit in one byte");
    uint8_t flat_index_[flat_index_size] = {};
    static_assert(wide_flat_limit < wide_flat_index_size &&
        wide_flat_limit <= UINT16_MAX, "wide staging positions fit the index");
    std::unique_ptr<uint16_t[]> wide_flat_index_;
    bool wide_flat_enabled_ = false;
    template<class Position>
    size_t flat_slot(uint64_t id, const Position* index, size_t mask) const;
    void clear_flat_index();
    const db::batch_entry* sorted_owner_ = nullptr;
    void reserve_sorted_pages(const db::batch_entry* root, size_t count);
    std::vector<page_change>::iterator sorted_page(uint64_t id) const;
    using staged_value = std::pair<const address, page_change>;
    std::map<address, page_change, std::less<address>,
        detail::page_node_allocator<staged_value>> changes_;
    // Track IDs inserted into changes_. Collisions and removed entries can
    // cause extra lookups. The map still resolves the full owner and ID.
    uint64_t staged_ids_[4] = {};
    bool may_have_staged_id(uint64_t id) const {
        return (staged_ids_[(id >> 6) & 3] & (uint64_t(1) << (id & 63))) != 0;
    }
    void remember_staged_id(uint64_t id) {
        staged_ids_[(id >> 6) & 3] |= uint64_t(1) << (id & 63);
    }
    bool failed_ = false;
    bool storage_error_ = false;
    db::page_lock_scope* scope_;
    std::string* page_owner_buffer_;
    // Keep the two most recent owners for alternating reads in Set algebra.
    // Both readers are pinned by this transaction's logical key locks.
    db::page_reader reader_;
    const db::batch_entry* reader_owner_ = nullptr;
    db::page_reader previous_reader_;
    const db::batch_entry* previous_reader_owner_ = nullptr;
    bool reader_supported_ = true;

    db::batch_entry* valid_address(const std::string& owner, uint64_t id);
    db::lookup_status lookup_impl(const std::string& owner, uint64_t id,
        std::string& out, bool reuse);
    // Native contexts obtained this stable entry from this transaction's
    // immutable root-key set. Flags and staged pages are still read live.
    bool valid_bound(db::batch_entry* root, uint64_t id) {
        if (!failed_ && root && id) return true;
        failed_ = true;
        return false;
    }
    bool first_matches(const db::batch_entry* root, uint64_t id) const {
        return first_.id == id &&
            (roots_.size() == 1 || first_.owner == root->key);
    }
    const page_change* staged_page(const db::batch_entry* root, uint64_t id) const;
    std::string* append_sorted_page(db::batch_entry* root, uint64_t id);
    std::string* staged_flat_write(uint64_t id);
    void spill_flat();
    // A malformed graph can alias an otherwise untouched sibling to a node
    // written during recursion. Its staged metadata must be read again.
    bool has_staged_bound(const db::batch_entry* root, uint64_t id) const {
        return id && (first_matches(root, id) ||
            (may_have_staged_id(id) && staged_page(root, id)));
    }
    db::lookup_status lookup_bound(db::batch_entry* root, uint64_t id,
        std::string& out, bool reuse = false);
    db::lookup_status lookup_page(const db::batch_entry* root,
        const std::string& owner, uint64_t id, std::string& out, bool reuse);
    db::lookup_status inspect_bound(db::batch_entry* root, uint64_t id,
        std::string& scratch, db::page_inspector visit, void* context);
    template<class Visitor>
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((always_inline))
#endif
    db::lookup_status inspect_bound_inline(db::batch_entry* root, uint64_t id,
        std::string& scratch, Visitor visit, void* context) {
        if (!valid_bound(root, id)) return db::lookup_status::error;
        if (root->action == db::update_action::erase)
            return db::lookup_status::missing;
        const page_change* staged = nullptr;
        if (first_matches(root, id)) staged = &first_;
        else if (may_have_staged_id(id)) staged = staged_page(root, id);
        if (staged) {
            if (staged->action == db::update_action::erase)
                return db::lookup_status::missing;
            visit(context, staged->value);
            return db::lookup_status::found;
        }
        if (root->clear_pages) return db::lookup_status::missing;
        if (reader_supported_ && reader_owner_ != root) {
            if (previous_reader_owner_ == root) {
                std::swap(reader_, previous_reader_);
                std::swap(reader_owner_, previous_reader_owner_);
            } else {
                previous_reader_ = reader_;
                previous_reader_owner_ = reader_owner_;
                if (scope_ && scope_->reader_available_) reader_ = scope_->reader_;
                else reader_supported_ = database_.dbbind_page_reader(root->key, reader_);
                reader_owner_ = root;
            }
        }
        db::lookup_status result;
        if (reader_supported_) {
            result = reader_.inspect(id, visit, context);
        } else {
            // Custom and persistent engines retain the checked, erased hook.
            db::page_inspection inspection(visit, context);
            scratch.clear();
            result = database_.dbinspect_page(root->key, id, scratch, inspection);
            if (!inspection.matches(result)) result = db::lookup_status::error;
        }
        if (result == db::lookup_status::error) {
            failed_ = storage_error_ = true;
            return result;
        }
        if (scope_ && reader_supported_ && !scope_->reader_available_ &&
            result == db::lookup_status::found) {
            scope_->reader_ = reader_;
            scope_->reader_available_ = true;
        }
        return result;
    }
    bool put_bound(db::batch_entry* root, uint64_t id,
        const std::string& bytes);
    bool put_bound(db::batch_entry* root, uint64_t id, std::string&& bytes);
    // Optimize ordered producers while allowing out-of-order fallback.
    bool put_tail_bound(db::batch_entry* root, uint64_t id, std::string&& bytes);
    bool erase_bound(db::batch_entry* root, uint64_t id);
    template <class Value>
    bool stage(const std::string& owner, uint64_t id, Value&& bytes,
        db::update_action action);
    template <class Value>
    bool stage_bound(db::batch_entry* root, uint64_t id, Value&& bytes,
        db::update_action action);
    template <class Value>
    bool stage_page(db::batch_entry* root, uint64_t id, Value&& bytes,
        db::update_action action);
    bool can_edit_list() const;
    bool has_changes() const {
        return first_.id != 0 || !flat_.empty() || !changes_.empty();
    }
    bool changes_owner(const std::string& owner) const;
    void clear_first();
    void materialize_first();
    static bool valid_roots(db::batch_view roots);
    bool valid_changes() const;
    std::vector<page_change> release_changes();
    // One-shot release into an empty destination owned by the active scope.
    void release_changes(std::vector<page_change>& out);
};

} // namespace pkv
