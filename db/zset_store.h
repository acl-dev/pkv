#pragma once
#include "map_store.h"
#include "coder/zset_page.h"
#include "common/zset_value.h"
#include "common/zset_range.h"
#include <memory>

namespace pkv {

// Two independently balanced trees in one private page family. The member
// index uses stable scan buckets; the score index uses lexical score/member
// keys. Both root pointers and the shared allocator commit in one PZR root.
class zset_store {
public:
    using status = map_store::status;
    zset_store(page_transaction& transaction, const std::string& owner);
    ~zset_store();
    zset_store(const zset_store&) = delete;
    zset_store& operator=(const zset_store&) = delete;
    bool open();
    bool open_live(long long now);
    // Treat expired roots as absent without staging an erase.
    bool open_snapshot(long long now);
    void initialize(int64_t expires_at_ms = -1);
    // Build both indexes from unique members, sorting the input by rank.
    bool build(std::vector<zset_member>& values, int64_t expires_at_ms);
    struct member_ref { const std::string* member; double score; };
    // Borrowed members remain live until the synchronous build returns.
    bool build_borrowed(std::vector<member_ref>& values, int64_t expires_at_ms);
    status error() const { return error_; }
    const zset_root& metadata() const { return root_; }
    bool exists() const { return root_.elements != 0; }
    bool get(const std::string& member, zset_score& out);
    // For a bounded batch of arguments borrowed from the live request.
    bool get_scores(const std::string* const* members, size_t count,
        zset_score* out);
    // Group distinct probes and validate matches against both indexes.
    bool get_scores_batched(const std::string* const* members, size_t count,
        zset_score* out);
    bool put(const std::string& member, double score, bool& added,
        bool& changed);
    bool increment(const std::string& member, double& score, bool& added,
        bool& changed);
    bool erase(const std::string& member, bool& removed);
    // The caller handles empty sets and whole-family removal.
    bool pop_edge(bool maximum, zset_member& out);
    bool pop_edges(uint64_t count, bool maximum, std::vector<zset_member>& out);
    // The DAO can pack the remainder when this is the final operation on
    // the opened tree. The caller still finishes the transaction normally.
    bool pop_rebuild(uint64_t count, bool maximum, std::vector<zset_member>& out,
        bool compact_remainder = false);
    bool rank(const std::string& member, zset_rank& out);
    bool select(uint64_t rank, zset_member& out, bool& found);
    static constexpr size_t sample_rank_batch = 8;
    // All ranks must exist. A fragmented path requests scalar fallback.
    // Output is usable only on success without fallback and includes duplicates.
    bool select_many(const uint64_t* ranks, size_t count, zset_member* out,
        bool& fallback);
    using visitor = std::function<bool(const std::string&, double)>;
    // May stop early. Callbacks must not mutate this context.
    bool visit(const visitor& callback);
    static constexpr size_t visit_batch_capacity = 32;
    using batch_visitor = std::function<size_t(const std::string* const*, size_t)>;
    // Visit validated members. The callback returns the next batch size, or
    // zero to stop. Member pointers remain live only during the callback.
    bool visit_batches(size_t batch_size, const batch_visitor& callback);
    bool visit_ordered(const visitor& callback);
    bool scan(const scan_options& options, scan_page& out);
    // False from the visitor stops successfully. Complete scans may validate
    // a bounded batch before invoking the visitor for its first member.
    bool visit_range(const zset_query& query, const visitor& callback,
        bool batch_members = false);
    bool range(const zset_query& query, std::vector<zset_member>& out);
    bool count(const zset_query& query, long long& out);
    bool clone_to(zset_store& destination, bool& copied);
    bool set_expiry(int64_t expires_at_ms);
    bool clear();
    bool finish();
private:
    page_transaction& transaction_;
    db::batch_entry* entry_;
    zset_root root_;
    map_root member_state_, score_state_;
    void use_crc_member_buckets(bool enabled);
    // Only destroyed contexts enter the bounded per-worker raw-block pool.
    // The stateless deleter preserves the original pointer-sized owners.
    struct tree_deleter {
        void operator()(map_store* value) const noexcept;
    };
    using tree_owner = std::unique_ptr<map_store, tree_deleter>;
    tree_owner members_, scores_;
    map_store* create_tree(map_root& state, bool lexical);
    status error_ = status::ok;
    bool dirty_ = false;
    bool fail(status error);
    enum class expiry_mode { ignore, hide, erase };
    bool open_impl(expiry_mode expiry, long long now);
    bool trees();
    bool tree_error();
    bool counts();
    bool erase_edge_member(std::string& key, zset_member& out, bool batch,
        unsigned member_bucket = 256);
    bool rebuild_remainder(const std::vector<zset_member>& values, size_t first,
        bool descending, int64_t expires_at_ms);
    template<class MemberAt>
    bool build_indexes(size_t count, int64_t expires_at_ms, const MemberAt& at);
    bool put_score(const std::string& member, double& score, bool increment,
        bool& added, bool& changed);
    db::page_lock_scope* repeated_read(const std::string& member,
        bool require_repeat = true) const;
    static bool reuse_score_key(const std::string& member);
    bool read_member_score(const std::string& member, double& score, bool& found);
    bool validate_score(const std::string& member, double score, zset_score& out);
    bool ordered_member(const std::string& key, zset_member& out,
        bool checked_score_key = false);
    bool bounds(const zset_query& query, uint64_t& first, uint64_t& last);
    bool lex_bounds(const zset_query& query, uint64_t& first, uint64_t& last,
        bool& indexed);
    bool collect_all(std::vector<zset_member>& values);
    bool validate_score_members(zset_member* members, size_t count);
    bool collect(uint64_t first, uint64_t last, bool reverse,
        std::vector<zset_member>& out);
    bool lex_range(const zset_query& query, std::vector<zset_member>* out,
        long long& count);
};

} // namespace pkv
