#pragma once
#include "page_transaction.h"
#include "node_read_workspace.h"
#include "node_key_workspace.h"
#include "map_index_workspace.h"
#include "coder/map_page.h"
#include "common/redis_scan.h"
#include <array>
#include <deque>
#include <unordered_set>

namespace pkv {

// Ordered native field/member index. AVL nodes, large keys and large values
// are independent bounded pages. Context lifetime is one logical-key callback.
class map_store {
public:
    enum class status {
        ok, not_paged, wrong_kind, invalid, unsupported, storage, too_large
    };
    map_store(page_transaction& transaction, const std::string& owner,
        map_kind kind = map_kind::hash, bool adaptive = true);
    // Internal trees share one owner's page allocator. The parent commits
    // metadata and resets the family; finish publishes only this tree state.
    map_store(page_transaction& transaction, const std::string& owner,
        map_root& state, uint64_t& sequence, bool lexical);
    ~map_store();
    map_store(const map_store&) = delete;
    map_store& operator=(const map_store&) = delete;
    bool open();
    bool open_live(long long now = -1);
    void initialize(int64_t expires_at_ms = -1);
    status error() const { return error_; }
    const map_root& metadata() const { return root_; }
    bool exists() const { return root_.elements != 0; }
    bool get(const std::string& key, std::string& value, bool& found);
    // An oversized value stays unread; exceeded distinguishes it from empty.
    bool get_bounded(const std::string& key, uint64_t limit,
        std::string& value, bool& found, bool& exceeded);
    bool length(const std::string& key, uint64_t& bytes, bool& found);
    bool hash_field(const std::string& field, std::string* value,
        uint64_t& bytes, bool& found);
    // Field pointers borrow the live request arguments.
    bool hash_fields(const std::string* const* fields, size_t count,
        std::string* values, unsigned& found);
    // Reuse bucket metadata for probes in one read-only Hash context.
    bool probe_hash_field(const std::string& field, std::string& value,
        bool& found);
    using hash_transform =
        std::function<bool(const std::string*, bool exceeded, std::string&)>;
    bool transform_hash_field(const std::string& key, uint64_t old_limit,
        const hash_transform& fn);
    // The member pointers borrow the live request arguments.
    bool set_membership(const std::string* const* members, size_t count,
        unsigned& found);
    // Repeated probes on one read-only Set context can reuse its directory.
    bool probe_set_member(const std::string& member, bool& found);
    // Zero-based order statistic; retrieves one member without full traversal.
    bool select(uint64_t rank, std::string& key, bool& found,
        std::string* value = nullptr);
    // Removes the member at a known in-range rank from a set.
    bool take_set_rank(uint64_t rank, std::string& key);
    // Fill a pre-sized sample with distinct, uniformly chosen members.
    bool take_set_sample(std::vector<std::string>& members);
    bool rank(const std::string& key, uint64_t& rank, bool& found);
    // Count keys before a bound; prefix bounds require a lexical tree.
    bool bound_rank(const std::string& key, bool upper, uint64_t& rank,
        bool prefix = false);
    bool put(const std::string& key, const std::string& value,
        bool only_missing, bool& added);
    // build_set calls the accessor once per index in ascending order.
    // The returned strings must remain valid for the whole call.
    using set_member_at = std::function<const std::string&(size_t)>;
    bool build_set(const set_member_at& member_at, size_t count, long long& added);
    using hash_field_refs =
        std::vector<std::pair<const std::string*, const std::string*>>;
    bool build_hash(const hash_field_refs& fields, int& added);
    bool erase(const std::string& key, bool& removed);
    bool insert_grouped_set(const set_member_at& member_at, size_t count,
        long long& added);
    bool erase_set(const set_member_at& member_at, size_t count, long long& removed);
    // Read the removed value during the deletion traversal.
    bool take(const std::string& key, std::string& value, bool& found);
    // Visitors may stop by returning false, but must not mutate this context.
    using visitor =
        std::function<bool(const std::string&, const std::string&)>;
    bool visit(const visitor& callback, bool with_values = true);
    using owning_visitor = std::function<bool(std::string&&, std::string&&)>;
    using key_filter = std::function<bool(const std::string&)>;
    bool visit_all(const owning_visitor& callback, bool with_values,
        const key_filter* filter = nullptr);
    using member_visitor = std::function<bool(const std::string&)>;
    bool visit_all_set_members(const member_visitor& callback);
    bool visit_set_prefix(const visitor& callback);
    bool visit_set_bucket(unsigned index, const member_visitor& callback);
    bool visit_rank(uint64_t first, uint64_t count, bool reverse,
        const visitor& callback, bool with_values = true);
    bool scan(const scan_options& options, scan_page& out);
    bool clone_to(map_store& destination, bool& copied);
    // Rebuild the index only; existing node IDs and blob pages stay owned.
    bool partition();
    bool set_expiry(int64_t expires_at_ms);
    bool clear();
    // pack_set is a terminal reduction: no further operations on this store.
    // The caller must hold a backend capability for compact Set roots.
    bool finish(bool pack_set = false);

private:
    friend class zset_store;
    map_store(page_transaction& transaction, db::batch_entry* entry,
        map_root& state, uint64_t& sequence, bool lexical);
    page_transaction& transaction_;
    const std::string owner_;
    const map_kind kind_;
    db::batch_entry* entry_;
    map_root root_;
    status error_ = status::ok;
    bool dirty_ = false;
    map_root* embedded_ = nullptr;
    uint64_t* sequence_ = nullptr;
    bool lexical_ = false;
    bool adaptive_ = false;
    bool probe_directory_ready_ = false;
    unsigned probe_groups_ = 0;
    unsigned bound_bucket_ = 256;
    detail::map_index_workspace index_;
    // Encoded nodes are decoded into owning values before the next read.
    detail::node_read_workspace node_bytes_;
    uint64_t last_id() const;
    unsigned bucket(const std::string& key) const;
    bool reuse_node_keys(const std::string& key) const;

    class blob_reader {
    public:
        blob_reader(map_store& store, const map_blob& blob);
        // The view remains valid until the next call on this reader.
        bool next(map_fragment_view& page, bool& present);
    private:
        map_store& store_;
        const map_blob& blob_;
        uint64_t id_;
        uint64_t offset_ = 0;
        bool inline_done_ = false;
        std::string bytes_;
    };
    class cursor {
    public:
        cursor(map_store& store, unsigned first = 0);
        bool next(map_node& node, bool& present);
    private:
        map_store& store_;
        const unsigned first_;
        std::deque<map_node> path_;
        bool started_ = false;
        uint64_t emitted_ = 0;
        bool descend(uint64_t id, uint32_t height, uint64_t elements);
    };
    class rank_cursor {
    public:
        rank_cursor(map_store& store, bool reverse);
        bool seek(uint64_t rank);
        bool next(map_node& node);
    private:
        map_store& store_;
        const bool reverse_;
        std::deque<map_node> path_;
        bool descend(uint64_t id, uint32_t height, uint64_t elements);
    };
    struct clone_state {
        std::unordered_set<uint64_t> visited;
        map_blob previous;
        unsigned bucket = 0;
        uint64_t elements = 0;
        uint64_t limit = 0;
    };
    bool clone_node(uint64_t id, map_store& destination, clone_state& state,
        unsigned depth);
    bool clone_blob(const map_blob& blob, map_store& destination,
        clone_state& state, unsigned* bucket = nullptr);
    bool compare_blobs(const map_blob& left, const map_blob& right, int& order);
    bool order(const map_node& node, const std::string& key, unsigned bucket,
        int& result);
    bool fail(status error);
    bool open_impl(bool expire, long long now);
    bool decoded(map_page_status result);
    bool allocate(uint64_t& id);
    bool read(uint64_t id, std::string& bytes, bool reuse = false);
    bool load(uint64_t id, map_node& node, bool with_value = true);
    struct inline_edit {
        const std::string& replacement;
        std::string& bytes;
        uint64_t id;
    };
    template<bool Ranked = false>
    bool find_inline(const std::string& key, unsigned bucket, map_blob& value,
        bool& found, bool& fallback, map_node* match = nullptr,
        const std::string* replacement = nullptr, inline_edit* edit = nullptr,
        uint64_t* rank = nullptr);
    bool rank_inline(const std::string& key, uint64_t& rank, bool& found,
        bool& fallback, bool require_empty);
    struct lookup_request {
        const std::string* key;
        size_t index;
        unsigned bucket;
    };
    using lookup_visitor = bool (*)(void*, size_t, const map_blob&);
    bool find_many(lookup_request* first, lookup_request* last,
        lookup_visitor visit, void* context);
    template<class Node>
    bool validate_node(uint64_t id, const Node& node, const char* key,
        size_t key_size) const;
    // Scratch for one forward traversal; never reused after a write.
    struct node_children {
        detail::node_key_workspace left, right;
        uint64_t retired_id = 0;
        uint64_t retired_key_head = 0;
        explicit node_children(bool reuse) : left(reuse), right(reuse) {}
        bool reuse() const { return left.enabled(); }
    };
    struct node_summary {
        uint32_t height;
        uint64_t elements;
        node_summary(uint32_t h = 0, uint64_t n = 0) : height(h), elements(n) {}
    };
    bool find_many_path(uint64_t id, uint64_t limit, uint32_t height,
        lookup_request* first, lookup_request* last,
        lookup_visitor visit, void* context, node_summary& result, map_node& node);
    bool visit_set_subtree(uint64_t id, uint32_t height, uint64_t elements,
        const member_visitor& callback, node_summary& summary);
    template<class Emit>
    bool visit_subtree(uint64_t id, uint32_t height, uint64_t elements,
        const Emit& emit, node_summary& summary, bool with_values);
    bool load_child(uint64_t id, map_node& node, node_children& children);
    bool write(const map_node& node);
    bool stats(uint64_t id, uint32_t& height, uint64_t& count,
        map_node* saved = nullptr);
    bool describe(const map_node& node, uint32_t& height, uint64_t& count,
        int& balance, node_children* children = nullptr);
    bool summarize(uint32_t left, uint32_t right, uint64_t before,
        uint64_t after, uint32_t& height, uint64_t& count, int& balance);
    bool check(const map_node& node, node_children* children = nullptr);
    bool check_path(const map_node& node, bool save_left,
        node_children& children, node_summary& left, node_summary& right);
    bool check_path(const map_node& node, bool save_left, map_node& next,
        node_summary& left, node_summary& right);
    bool check_root();
    bool inspect(uint64_t id, std::string& scratch,
        db::page_inspector visit, void* context);
    template<class Visitor>
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((always_inline))
#endif
    bool inspect(uint64_t id, std::string& scratch, Visitor visit, void* context) {
        if (!id || id > last_id()) return fail(status::invalid);
        const auto result = transaction_.inspect_bound_inline(entry_, id,
            scratch, visit, context);
        if (result == db::lookup_status::found) return true;
        return fail(result == db::lookup_status::error ? status::storage :
            status::invalid);
    }
    struct set_pop_groups {
        std::unique_ptr<std::array<map_group, 16>> pages;
        unsigned changed = 0;
    };
    bool read_directory(bool full = true);
    bool read_group(unsigned index, const set_pop_groups* pop = nullptr);
    bool read_group_page(const map_root& root, unsigned tag, map_group& out,
        bool& cached_copy);
    bool write_group_page(const map_root& root, unsigned tag,
        const map_group& group);
    bool write_groups();
    bool update_group(unsigned index, const map_root& state,
        uint64_t old_count, set_pop_groups* pop);
    bool write_directory();
    using bucket_operation = std::function<bool(map_store&)>;
    bool bucket_run(unsigned index, const bucket_operation& callback,
        bool refresh = true, set_pop_groups* pop = nullptr);
    bool bucket_visit(const visitor& callback, bool with_values,
        bool lazy_groups = false);
    bool bucket_scan(const scan_options& options, scan_page& out);
    bool bucket_select(uint64_t rank, std::string& key, bool& found,
        std::string* value);
    bool bucket_rank(const std::string& key, uint64_t& rank, bool& found);
    bool rank_in_tree(const std::string& key, uint64_t& rank, bool& found,
        bool require_empty);
    bool select_score_key(uint64_t rank, std::string& key, bool& found);
    bool select_score_path(uint64_t id, uint64_t rank, std::string& key,
        node_children& children, unsigned depth);
    bool visit_score_range(uint64_t first, uint64_t last, bool reverse,
        const visitor& callback);
    bool visit_score_path(const map_node& node, uint64_t first, uint64_t last,
        bool reverse, const std::string* lower, const std::string* upper,
        const visitor& callback);
    bool bucket_bound(const std::string& key, bool upper, uint64_t& rank);
    bool bucket_range(uint64_t first, uint64_t count, bool reverse,
        const visitor& callback, bool with_values);
    bool bucket_clone(map_store& destination, bool& copied);
    bool rebuild_bucket(std::vector<map_node>& nodes, size_t first,
        size_t end, uint64_t& tree, uint32_t& height);
    struct bulk_member {
        const std::string* key;
        const std::string* value;
        unsigned bucket;
        static bool less(const bulk_member& a, const bulk_member& b) {
            if (a.bucket != b.bucket) return a.bucket < b.bucket;
            return *a.key < *b.key;
        }
    };
    bool build_tree(const bulk_member* members, size_t count,
        uint64_t& tree, uint32_t& height, map_node& node);
    bool store_bulk_blob(const std::string& input, size_t limit, map_blob& blob) {
        if (input.size() > limit) return store_blob(input, limit, blob);
        blob.inline_value.assign(input);
        blob.head = 0;
        blob.length = input.size();
        return true;
    }
    template<class Fill>
    bool build_tree_from(size_t first, size_t count, uint64_t& tree,
        uint32_t& height, map_node& node, const Fill& fill) {
        tree = 0; height = 0;
        if (!count) return true;
        const auto middle = count / 2;
        uint64_t left_id, right_id;
        uint32_t left, right;
        if (!build_tree_from(first, middle, left_id, left, node, fill) ||
            !build_tree_from(first + middle + 1, count - middle - 1,
                right_id, right, node, fill)) return false;
        node.left = left_id;
        node.right = right_id;
        node.height = std::max(left, right) + 1;
        node.elements = count;
        if (!allocate(node.id) || !fill(first + middle, node)) return false;
        // Only ordered appends can encode directly into the final page.
        if (auto* bytes = transaction_.append_sorted_page(entry_, node.id)) {
            if (!decoded(map_page_codec::encode_packed_node(node, *bytes, !lexical_)))
                return false;
        } else if (!write(node)) return false;
        tree = node.id; height = node.height;
        return true;
    }
    template<class Member, class ByKey>
#if defined(__GNUC__)
    __attribute__((noinline))
#endif
    static void sort_member_buckets(std::vector<Member>& ordered, const ByKey& by_key) {
        // Each member already has a bucket. Partition the references in
        // place, then compare keys only against members in the same bucket.
        std::array<size_t, 256> ends{};
        for (const auto& member : ordered) ++ends[member.bucket];
        std::array<size_t, 256> next;
        size_t end = 0;
        for (size_t i = 0; i < ends.size(); ++i) {
            next[i] = end;
            end += ends[i];
            ends[i] = end;
        }
        for (size_t i = 0; i < ends.size(); ++i) {
            // Earlier buckets may have already placed some of our entries.
            while (next[i] < ends[i]) {
                auto& member = ordered[next[i]];
                if (member.bucket == i) ++next[i];
                else std::swap(member, ordered[next[member.bucket]++]);
            }
            const auto begin = i ? ends[i - 1] : 0;
            if (ends[i] - begin > 1)
                std::sort(ordered.begin() + begin, ordered.begin() + ends[i],
                    by_key);
        }
    }
    bool build_ordered(std::vector<bulk_member>& ordered);
    static constexpr size_t hash_partition_size = 129;
    static constexpr size_t set_partition_size = 257;
    bool find(const std::string& key, map_node& node, bool& found);
    bool compare(const map_blob& key, const std::string& input, int& order);
    bool prefix_order(const map_blob& key, const std::string& input,
        int& order);
    using fragment_visitor = std::function<bool(const map_fragment_view&)>;
    bool visit_blob(const map_blob& blob, const fragment_visitor& callback);
    bool store_blob(const std::string& input, size_t limit, map_blob& out,
        uint64_t retired_head = 0);
    bool read_blob(const map_blob& blob, std::string& out);
    bool pack_set_root(bool& packed);
    bool collect_packed_set(std::vector<std::string>& members,
        std::unordered_set<uint64_t>& seen, size_t& payload, bool& eligible);
    bool erase_blob(const map_blob& blob) { return update_fragments(blob, nullptr); }
    bool update_fragments(const map_blob& blob, const std::string* replacement);
    bool make_node(const std::string& key, const std::string& value,
        unsigned bucket, uint64_t& id, const node_children& children);
    // Synchronous old-value check before replacement or deletion.
    // The callback may read another tree but must not mutate this context.
    // Returning false aborts the transaction; replace=false is a no-op.
    struct value_gate {
        using check = bool (*)(void*, const map_blob&, bool&);
        using transform = bool (*)(void*, const map_blob*, std::string&);
        value_gate(check inspect, void* context, transform make = nullptr)
            : inspect(inspect), context(context), make(make) {}
        check inspect;
        void* context;
        transform make;
    };
    bool update_inline(const std::string& key, const std::string& value,
        const value_gate& gate, bool& handled);
    bool put_checked(const std::string& key, const std::string& value,
        bool only_missing, bool& added, const value_gate* gate);
    bool put_in_tree(const std::string& key, const std::string& value,
        unsigned bucket, bool only_missing, bool& added, const value_gate* gate);
    // updated is consumed only when a new entry was added successfully.
    bool insert(uint64_t& id, const std::string& key, const std::string& value,
        bool only_missing, bool& added, unsigned bucket, unsigned depth,
        node_children& children, node_summary& updated, const value_gate* gate);
    bool remove(uint64_t& id, const std::string& key, bool& removed,
        unsigned bucket, unsigned depth, node_children& children,
        node_summary& updated, std::string* value = nullptr,
        const value_gate* gate = nullptr);
    bool erase_value(const std::string& key, bool& removed,
        std::string* value, const value_gate* gate = nullptr,
        unsigned key_bucket = 256);
    bool replace_score_key(const std::string& old_key, const std::string& new_key);
    bool replace_score_path(uint64_t& id, const std::string& old_key,
        const std::string& new_key, unsigned depth, node_children& children,
        node_summary& updated);
    bool take_edge(bool maximum, std::string& key);
    bool take_edges(size_t count, bool maximum, std::vector<std::string>& keys);
    bool take_edge_range(uint64_t& id, node_summary& summary, size_t count,
        bool maximum, std::vector<std::string>& keys);
    bool join_edge_range(map_node& pivot, uint64_t& id, node_summary left,
        node_summary right, node_summary& summary);
    bool set_bucket_at_rank(uint64_t& rank, unsigned& bucket,
        const set_pop_groups* pop = nullptr);
    bool take_bucket_rank(uint64_t rank, std::string& key,
        set_pop_groups* pop = nullptr);
    bool finish_set_pop(const set_pop_groups& pop);
    bool remove_rank(uint64_t& id, uint64_t rank, std::string& key,
        unsigned depth, node_children& children, node_summary& updated);
    bool remove_node(map_node& node, uint64_t& id, unsigned depth,
        node_children& children, node_summary left, node_summary right,
        node_summary& updated);
    template<bool Maximum>
    bool remove_edge(uint64_t& id, map_node& edge, unsigned depth,
        node_children& children, node_summary& updated,
        std::string* key = nullptr);
    bool rebalance_known(map_node& node, uint64_t& id, node_summary left,
        node_summary right, bool changed_left, node_summary& updated,
        bool reuse, bool same_links = false);
    bool rebalance(map_node& node, uint64_t& id, int balance,
        node_summary before, node_summary after, node_summary& updated, bool reuse);
    bool rotate(map_node& node, map_node& pivot, bool left, bool intermediate,
        node_summary outer, node_summary& pivot_left, node_summary& pivot_right);
};

} // namespace pkv
