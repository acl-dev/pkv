#pragma once
#include "page_transaction.h"
#include "coder/map_page.h"
#include "common/redis_scan.h"
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
        map_kind kind = map_kind::hash);
    // Internal trees share one owner's page allocator. The parent commits
    // metadata and resets the family; finish publishes only this tree state.
    map_store(page_transaction& transaction, const std::string& owner,
        map_root& state, uint64_t& sequence, bool lexical);
    ~map_store();
    map_store(const map_store&) = delete;
    map_store& operator=(const map_store&) = delete;
    bool open();
    void initialize(int64_t expires_at_ms = -1);
    status error() const { return error_; }
    const map_root& metadata() const { return root_; }
    bool exists() const { return root_.elements != 0; }
    bool get(const std::string& key, std::string& value, bool& found);
    // An oversized value stays unread; exceeded distinguishes it from empty.
    bool get_bounded(const std::string& key, uint64_t limit,
        std::string& value, bool& found, bool& exceeded);
    bool length(const std::string& key, uint64_t& bytes, bool& found);
    // Zero-based order statistic; retrieves one member without full traversal.
    bool select(uint64_t rank, std::string& key, bool& found,
        std::string* value = nullptr);
    bool rank(const std::string& key, uint64_t& rank, bool& found);
    // Count keys before a bound; prefix bounds require a lexical tree.
    bool bound_rank(const std::string& key, bool upper, uint64_t& rank,
        bool prefix = false);
    bool put(const std::string& key, const std::string& value,
        bool only_missing, bool& added);
    bool erase(const std::string& key, bool& removed);
    // Read the removed value during the deletion traversal.
    bool take(const std::string& key, std::string& value, bool& found);
    // Visitors may stop by returning false, but must not mutate this context.
    using visitor =
        std::function<bool(const std::string&, const std::string&)>;
    bool visit(const visitor& callback, bool with_values = true);
    bool visit_rank(uint64_t first, uint64_t count, bool reverse,
        const visitor& callback, bool with_values = true);
    bool scan(const scan_options& options, scan_page& out);
    bool clone_to(map_store& destination, bool& copied);
    bool set_expiry(int64_t expires_at_ms);
    bool clear();
    bool finish();

private:
    friend class zset_store;
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
    uint64_t last_id() const;
    unsigned bucket(const std::string& key) const;

    class blob_reader {
    public:
        blob_reader(map_store& store, const map_blob& blob);
        bool next(map_fragment& page, bool& present);
    private:
        map_store& store_;
        const map_blob& blob_;
        uint64_t id_;
        uint64_t offset_ = 0;
        bool inline_done_ = false;
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
    };
    bool clone_node(uint64_t id, map_store& destination, clone_state& state,
        unsigned depth);
    bool clone_blob(const map_blob& blob, map_store& destination,
        clone_state& state, unsigned* bucket = nullptr);
    bool compare_blobs(const map_blob& left, const map_blob& right, int& order);
    bool order(const map_node& node, const std::string& key, unsigned bucket,
        int& result);
    bool fail(status error);
    bool decoded(map_page_status result);
    bool allocate(uint64_t& id);
    bool read(uint64_t id, std::string& bytes);
    bool load(uint64_t id, map_node& node);
    bool write(const map_node& node);
    bool stats(uint64_t id, uint32_t& height, uint64_t& count);
    bool refresh(map_node& node, int& balance);
    bool check(const map_node& node);
    bool check_root();
    bool find(const std::string& key, map_node& node, bool& found);
    bool compare(const map_blob& key, const std::string& input, int& order);
    bool prefix_order(const map_blob& key, const std::string& input,
        int& order);
    using fragment_visitor = std::function<bool(const map_fragment&)>;
    bool visit_blob(const map_blob& blob, const fragment_visitor& callback);
    bool store_blob(const std::string& input, size_t limit, map_blob& out);
    bool read_blob(const map_blob& blob, std::string& out);
    bool erase_blob(const map_blob& blob);
    bool make_node(const std::string& key, const std::string& value,
        unsigned bucket, uint64_t& id);
    bool insert(uint64_t& id, const std::string& key, const std::string& value,
        bool only_missing, bool& added, unsigned bucket, unsigned depth);
    bool remove(uint64_t& id, const std::string& key, bool& removed,
        unsigned bucket, unsigned depth, std::string* value = nullptr);
    bool erase_value(const std::string& key, bool& removed,
        std::string* value);
    bool remove_min(uint64_t& id, map_node& minimum, unsigned depth);
    bool rebalance(map_node& node, uint64_t& id);
    bool rotate(uint64_t& id, bool left, bool intermediate = false);
};

} // namespace pkv
