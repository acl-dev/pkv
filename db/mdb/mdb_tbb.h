#pragma once
#if defined(HAS_TBB)
#include "../db.h"
#include <tbb/concurrent_hash_map.h>
#include <acl-lib/fiber/fiber_mutex.hpp>
#include <array>
#include <atomic>
#include "page_index.h"
#include "common/dense_key_index.h"

namespace pkv {

class mdb_tbb : public db {
public:
    explicit mdb_tbb(size_t count);
    ~mdb_tbb() override;
    // Called by background maintenance outside execution admission.
    bool trim_idle_storage(uint64_t now_ms);
protected:
    bool dbopen(const char*) override;
    bool dbset(const std::string& key, const std::string& value) override;
    bool dbget(const std::string& key, std::string& value) override;
    bool dbinline_strings() const override { return true; }
    inline_status dbread_inline(const std::string& key,
        std::string& out, size_t offset, size_t limit) override;
    bool dbdel(const std::string& key) override;
    lookup_status dblookup(const std::string& key, std::string& value) override;
    db_cursor* dbcreate_cursor() override { return nullptr; }
    bool dbscan(size_t, db_cursor&, std::vector<std::string>&, size_t) override;
    count_status dbkey_count(uint64_t& out) override;
    scan_status dbsample_key(std::string& out, bool& found) override;
    scan_status dbkey_at_rank(uint64_t rank, std::string& out,
        bool& found) override;
    scan_status dbvisit_keys(const key_visitor& visit) override;
    scan_status dbvisit_root_records(const root_visitor& visit) override;
    scan_status dbscan_keys(unsigned long long cursor, size_t count,
        std::vector<std::string>& keys, unsigned long long& next) override;
    scan_status dbscan_root_page(unsigned long long cursor, size_t count,
        unsigned long long& next, const scan_root_visitor& visit,
        const key_visitor& filter) override;
    batch_status dbapply(const std::vector<batch_entry>& entries) override;
    bool dbsupports_pages() const override { return true; }
    bool dbsupports_prepare() const override { return true; }
    bool dbsupports_owned_prepare() const override { return true; }
    batch_status dbprepare_owned_pages(const std::vector<batch_entry>& roots,
        const std::vector<page_change>& pages,
        std::unique_ptr<prepared_batch>& out) override;
    bool dbsupports_family_transfer(const db& other) const override;
    bool dbsupports_detached_pages() const override { return true; }
    batch_status dbdetach_pages(const std::string& owner,
        std::unique_ptr<prepared_pages>& out) override;
    lookup_status dblookup_bounded_root(const std::string& owner,
        std::string& out) override;
    bool dbsupports_page_stream() const override { return true; }
    batch_status dbvisit_pages(const std::string& owner,
        const page_visitor& visit) override;
    batch_status dbprepare_pages(const std::vector<batch_entry>& roots,
        const std::vector<page_change>& pages,
        prepare_callback& ready) override;
    lookup_status dblookup_page(const std::string& owner, uint64_t id,
        std::string& value) override;
    bool dbbind_page_reader(const std::string& owner, page_reader& reader) override;
    batch_status dbapply_pages(const std::vector<batch_entry>& roots,
        const std::vector<page_change>& pages) override;
    batch_status dbadopt_page_family(const std::vector<batch_entry>& roots,
        std::vector<page_change>& pages) override;
    size_t dbsize() const override { return shard_count_; }
    const char* get_dbtype() const override { return "mdb_tbb"; }
private:
    friend class db;
    static const size_t scan_buckets = 256;
    static constexpr size_t scan_lock_count = 8;
    // Keep eight stripes within each shard while bounding the total number of mutexes.
    static constexpr size_t scan_lock_groups = 16;
    static constexpr size_t index_word_bits = 16;
    // Each populated oneTBB table allocates an initial bucket block.
    static constexpr size_t owner_table_count = 8;
    static_assert(scan_buckets % index_word_bits == 0, "complete index words");
    static_assert(scan_buckets <= UINT8_MAX + 1u, "batch slot fits one byte");
    class page_map {
        using index = mdb_page_index_impl<false>;
        std::unique_ptr<index> values_;
    public:
        // Only records with physical pages need an index allocation.
        bool empty() const noexcept { return !values_; }
        size_t size() const { return values_ ? values_->size() : 0; }
        bool has_small_pages(size_t count) const {
            return values_ && values_->has_small_pages(count);
        }
        const std::string* get(uint64_t id) const {
            return values_ ? values_->get(id) : nullptr;
        }
        std::string* get(uint64_t id) {
            return values_ ? values_->get(id) : nullptr;
        }
        static size_t sparse_tail_offset(size_t consecutive) {
            return index::sparse_tail_offset(consecutive);
        }
        template<class Iterator, class Value>
        void build_consecutive(Iterator first, size_t count, Value value) {
            std::unique_ptr<index> created(new index);
            created->build_consecutive(first, count, value);
            values_.swap(created);
        }
        void emplace(uint64_t id, const std::string& value, size_t consecutive = 0,
            bool grow_tail = true) {
            if (values_) {
                values_->emplace(id, value, grow_tail);
                return;
            }
            std::unique_ptr<index> created(new index);
            created->reserve_dense_prefix(consecutive);
            // Attach only after the first insertion succeeds. Rollback owns
            // subsequent insertions and releases the index when it is empty.
            created->emplace(id, value);
            values_.swap(created);
        }
        void erase(uint64_t id, bool reclaim = false) noexcept {
            if (!values_) return;
            values_->erase(id, reclaim);
            if (values_->empty()) values_.reset();
        }
        void reclaim_prefix() noexcept {
            if (values_) values_->reclaim_prefix();
        }
        void swap(page_map& other) noexcept { values_.swap(other.values_); }
        void promote_prefix(size_t additions) noexcept {
            if (values_) values_->promote_prefix(additions);
        }
        void promote_tail(uint64_t last_id) noexcept {
            if (values_) values_->promote_tail(last_id);
        }
        template<class Visitor>
        bool visit(Visitor visit_page) const {
            return !values_ || values_->visit(visit_page);
        }
    };
    struct record { std::string root; page_map pages; };
    static page_reader record_reader(const record* owner);
    const std::string* borrow_root(const std::string& key, page_reader& reader);
    bool insert_root(const std::string& key, std::string& value);
    batch_status edit_bound_zset_pages(const batch_entry& root,
        const std::vector<page_change>& pages, const page_reader& reader);
    batch_status replace_bound_root(const batch_entry& root, const page_reader& reader);
    batch_status replace_bound_page(const batch_entry& root,
        const page_change& page, const page_reader& reader, std::string* adopted);
    static batch_status edit_bound_list_page(const batch_entry& root,
        const list_page_edit& edit, const page_reader& reader);
    static batch_status replace_existing_page(const batch_entry& root,
        const page_change& page, record* owner, std::string* adopted = nullptr);
    struct owner_entry {
        // The logical key lock protects the record, including its lifetime.
        mutable record data;
        mutable size_t position = 0;
    };
    using owner_value = std::pair<const std::string, owner_entry>;
    using owner_map = tbb::concurrent_hash_map<std::string, owner_entry,
        tbb::tbb_hash_compare<std::string>, std::allocator<owner_value>>;
    // Read or update the tagged slot under its stripe, or under exclusive
    // admission. Owner nodes have stable addresses while indexed.
    class scan_index {
    public:
        using pointer = const owner_value*;
        scan_index() = default;
        ~scan_index() { delete overflow(); }
        scan_index(const scan_index&) = delete;
        scan_index& operator=(const scan_index&) = delete;
        size_t size() const {
            const auto* many = overflow();
            return many ? many->size() : size_t(value_ != 0);
        }
        pointer operator[](size_t position) const {
            const auto* many = overflow();
            return many ? (*many)[position] : single();
        }
        void prepare();
        void publish(pointer node) noexcept;
        void cancel() noexcept;
        void erase(pointer node) noexcept;
    private:
        struct multiple {
            size_t size() const { return dense_ ? dense_->size() : inline_size(); }
            pointer operator[](size_t position) const {
                return dense_ ? (*dense_)[position] : inline_[position];
            }
            void prepare();
            void publish(pointer node) noexcept;
            void cancel() noexcept;
            void erase(pointer node) noexcept;
            void shrink_dense(size_t limit) noexcept;
            using dense_index = dense_key_index<owner_value>;
            std::array<pointer, 2> inline_{};
            std::unique_ptr<dense_index> dense_;
            size_t inline_size() const {
                return inline_[1] ? 2 : inline_[0] ? 1 : 0;
            }
        };
        static constexpr uintptr_t overflow_tag = 1;
        static_assert(alignof(owner_value) > overflow_tag &&
            alignof(multiple) > overflow_tag, "scan pointer tag needs alignment");
        uintptr_t value_ = 0;
        multiple* overflow() const noexcept {
            return value_ & overflow_tag ?
                reinterpret_cast<multiple*>(value_ & ~overflow_tag) : nullptr;
        }
        pointer single() const noexcept { return reinterpret_cast<pointer>(value_); }
        void collapse(multiple& many, size_t limit) noexcept;
    };
    struct scan_slots {
        std::array<scan_index, index_word_bits> indexes;
        std::atomic<uint64_t> live{0};
    };
    struct store {
        owner_map& owners;
        std::atomic<size_t> owner_count{0};
        std::array<std::atomic<scan_slots*>, scan_buckets / index_word_bits> scan_pages;
        explicit store(owner_map& table);
        ~store();
        size_t size() const noexcept { return owner_count.load(std::memory_order_relaxed); }
    };
    struct location { size_t shard; size_t bucket; };
    const size_t shard_count_;
    // Shards remain stable under shared admission.
    std::unique_ptr<std::atomic<store*>[]> stores_;
    // SCAN partitions keep their cursors and share TBB owner tables.
    std::array<std::atomic<owner_map*>, owner_table_count> owner_tables_;
    std::array<std::atomic<acl::fiber_mutex*>, scan_lock_groups * scan_lock_count> scan_locks_;
    std::atomic<uint64_t> root_count_{0};
    std::atomic<uint64_t> empty_since_ms_{0};
    std::atomic<size_t> sample_shard_bound_{0};
    // Accessed only under exclusive sampling admission.
    unsigned sample_bound_misses_ = 0;
    store* shard_at(size_t shard) const noexcept {
        return stores_[shard].load(std::memory_order_acquire);
    }
    template<class Visit>
    static bool visit_indexes(const store& shard, size_t first, const Visit& visit);
    static const owner_value* owner_at_rank(const store& shard, uint64_t rank);
    template<class Visit>
    scan_status walk_scan_page(unsigned long long cursor, size_t count,
        unsigned long long& next, const Visit& visit);
    location locate(const std::string& key) const;
    static size_t scan_stripe(const location& position) noexcept {
        return (position.shard % scan_lock_groups) * scan_lock_count +
            position.bucket % scan_lock_count;
    }
    acl::fiber_mutex& scan_lock(const location& position);
    scan_index* index(const location& position, bool create);
    // Caller holds the scan stripe for the bucket.
    static void mark_scan_slot(store& shard, size_t bucket, bool live) noexcept;
    void erase_scan_key(store& shard, size_t bucket, scan_index& index,
        const owner_value* owner) noexcept;
    const owner_value* add_owner(store& shard, size_t bucket, scan_index& index,
        const std::string& key);
    record* find(const std::string& key,
        const owner_value** canonical = nullptr);
    batch_status commit(const std::vector<batch_entry>& roots,
        const std::vector<page_change>& pages, bool preserve_pages,
        prepare_callback* ready = nullptr);
    struct commit_state;
};

} // namespace pkv
#endif
