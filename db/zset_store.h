#pragma once
#include "map_store.h"
#include "coder/zset_page.h"
#include "common/zset_value.h"
#include "common/zset_range.h"

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
    void initialize(int64_t expires_at_ms = -1);
    status error() const { return error_; }
    const zset_root& metadata() const { return root_; }
    bool exists() const { return root_.elements != 0; }
    bool get(const std::string& member, zset_score& out);
    bool put(const std::string& member, double score, bool& added,
        bool& changed);
    bool erase(const std::string& member, bool& removed);
    bool rank(const std::string& member, zset_rank& out);
    bool select(uint64_t rank, zset_member& out, bool& found);
    using visitor = std::function<bool(const std::string&, double)>;
    // May stop early. Callbacks must not mutate this context.
    bool visit(const visitor& callback);
    bool visit_ordered(const visitor& callback);
    bool scan(const scan_options& options, scan_page& out);
    // Stream an indexed range; false from the visitor stops successfully.
    bool visit_range(const zset_query& query, const visitor& callback);
    bool range(const zset_query& query, std::vector<zset_member>& out);
    bool count(const zset_query& query, long long& out);
    bool clone_to(zset_store& destination, bool& copied);
    bool set_expiry(int64_t expires_at_ms);
    bool clear();
    bool finish();
private:
    page_transaction& transaction_;
    const std::string owner_;
    db::batch_entry* entry_;
    zset_root root_;
    map_root member_state_, score_state_;
    std::unique_ptr<map_store> members_, scores_;
    status error_ = status::ok;
    bool dirty_ = false;
    bool fail(status error);
    bool trees();
    bool tree_error();
    bool counts();
    bool ordered_member(const std::string& key, zset_member& out);
    bool bounds(const zset_query& query, uint64_t& first, uint64_t& last);
    bool lex_bounds(const zset_query& query, uint64_t& first, uint64_t& last,
        bool& indexed);
    bool collect(uint64_t first, uint64_t last, bool reverse,
        std::vector<zset_member>& out);
    bool lex_range(const zset_query& query, std::vector<zset_member>* out,
        long long& count);
};

} // namespace pkv
