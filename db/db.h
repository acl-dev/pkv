#pragma once
#include <cstdint>
#include <string>
#include <memory>
#include <vector>
#include <functional>
#include <algorithm>
#include "c++_patch.h"
#include "common/execution_gate.h"
#include "watch_registry.h"
#include "common/expiry_progress.h"
#include "common/key_wait.h"

namespace acl { class fiber_mutex; }

namespace pkv {

/**
 * @brief The abstract base class for key-value database.
 * 
 */
class db;
using shared_db = std::shared_ptr<db>;

class record_codec;
class db_cursor;
class db_watchers;
class family_publication;
struct family_move;
class page_transaction;
class page_workspace;
class move_recovery;
class expiry_index;
class key_wait_registry;
class key_wait_subscription;
struct page_change;
struct page_update;

/**
 * @brief A class representing a key-value database.
 * 
 * This class provides methods for opening a database file, setting and getting
 * values for keys, deleting keys, creating a cursor for iterating over the keys
 * in the database, and scanning the database using a cursor. It also provides
 * methods for creating different types of databases such as rocksdb,
     wiredtiger,
 * memory-mapped database with hash table, AVL tree, and TBB tree.
 */
class db {
public:
    /**
     * @brief Default constructor.
     */
    db();

    /**
     * @brief Virtual destructor.
     */
    virtual ~db();

    // Capability hints exclude size/record quotas; commit validates them again.
    bool supports_native_pages() const { return dbsupports_pages(); }
    // Engine capability only; the coordinator still validates the full pair.
    bool supports_family_transfer(const db& target) const {
        return dbsupports_family_transfer(target) &&
            target.dbsupports_family_transfer(*this);
    }
    bool allows_page_writes() const;
    // Source EXEC can stage only engines with one atomic commit domain.
    bool supports_workspace() const {
        return dbsupports_pages() && dbatomic_workspace();
    }
protected:
    // Actor-local views have one private key mutex. Ordinary databases retain
    // all 256 stripes and their existing concurrent command admission.
    explicit db(bool actor_local);
public:
    enum class sync_status { ok, unsupported, error };
    // Fence the owner's physical WAL, including a deleted/missing owner.
    // Holds its ordinary key lock; does not read values or publish changes.
    // Later writes are not covered once this call releases that lock.
    // Do not recurse from a transaction callback. This is not Redis SYNC
    // or a cross-engine transaction.
    sync_status sync_key(const std::string& owner);
    // Terminal process shutdown only. Caller holds exclusive admission through
    // process exit; no command may access these stores after this call.
    bool close_for_exit();
    enum class lookup_status { found, missing, error };
    enum class inline_status { found, missing, fallback, error };
    // Bounded native String read under ordinary admission and key locks.
    // Other types, expired roots and unsupported engines use page transactions.
    inline_status read_inline_string(const std::string& key, std::string& out);
    enum class update_action { keep, write, erase };
    using update_callback = std::function<update_action(bool, std::string&)>;

    enum class count_status { ok, unsupported, error };
    // One coherent stored-root count, independent of TTL decoding and pages.
    count_status key_count(uint64_t& out);
    count_status keyspace(uint64_t& keys, uint64_t& expires,
        uint64_t& average);
    enum class scan_status { ok, unsupported, error };
    // Stored roots only, with unspecified rank order. Exclusive admission
    // keeps the count, selection and borrowed visitor keys coherent.
    scan_status key_at_rank(uint64_t rank, std::string& out, bool& found);
    scan_status sample_key(std::string& out, bool& found);
    // Uniform reservoir over live roots at one exclusive command clock.
    // Never removes expired roots or materializes a vector of all keys.
    scan_status sample_live_key(std::string& out, bool& found);
    using key_visitor = std::function<bool(const std::string&)>;
    scan_status rebuild_expiry();
    expiry_progress expire_step(size_t limit);
    // Stateless key pages. Backends copy keys under their native locks and
    // release those locks before logical expiry/type filtering takes key locks.
    scan_status scan_keys(unsigned long long cursor, size_t count,
            std::vector<std::string>& keys, unsigned long long& next) {
        execution_guard admission;
        keys.clear(); next = 0;
        return dbscan_keys(cursor, count, keys, next);
    }
protected:
    // Exclusive admission stabilizes key references. Callbacks may inspect
    // raw root metadata through dblookup, but must not mutate a DB or call
    // its public locking APIs. false stops visitation; no key is retained.
    virtual scan_status dbvisit_keys(const key_visitor&) {
        return scan_status::unsupported;
    }
    virtual scan_status dbkey_at_rank(uint64_t rank, std::string& out,
        bool& found);
    virtual count_status dbkey_count(uint64_t&) {
        return count_status::unsupported;
    }
    virtual scan_status dbscan_keys(unsigned long long, size_t,
        std::vector<std::string>&, unsigned long long&) {
        return scan_status::unsupported;
    }
public:
    enum class batch_status { ok, aborted, unsupported, error };
    // A prepared engine holds its native locks and rolls back on destruction.
    // Inputs outlive the callback. Publication cannot fail or allocate.
    class prepared_transfer {
    public:
        virtual ~prepared_transfer() = default;
        // Exactly once, before publishing either participating batch.
        virtual void publish() noexcept = 0;
    };
    // Cold ownership plans detach native maps under exclusive admission.
    // Destroy native batches before these handles to restore on rollback.
    class prepared_pages {
    public:
        virtual ~prepared_pages() = default;
        virtual std::unique_ptr<prepared_pages> fresh() const = 0;
        virtual batch_status prepare(const std::vector<page_change>&) = 0;
        virtual void publish() noexcept = 0;
    };
    class prepared_batch {
    public:
        virtual ~prepared_batch() = default;
        virtual void publish() noexcept = 0;
        // Prepare ownership handoff without moving pages yet. Both batches
        // retain locks; a discarded handle leaves normal rollback intact.
        virtual std::unique_ptr<prepared_transfer> prepare_family(
            prepared_batch&) { return nullptr; }
        virtual std::unique_ptr<prepared_transfer> prepare_family(
            const std::string&, prepared_pages&) { return nullptr; }
    };
    class prepare_callback {
    public:
        virtual ~prepare_callback() = default;
        virtual batch_status prepared(prepared_batch& batch) = 0;
    };
    batch_status expire_keys(std::vector<std::string> keys, size_t& removed);
    struct batch_entry {
        const std::string key;
        const bool found;
        std::string value;
        update_action action = update_action::keep;
        bool clear_pages = false;
        bool maintenance_only = false;
        // Internal ordered-replay history; not a standalone wire flag.
        bool force_watch_invalidation = false;
        batch_entry(const std::string& name, bool present, std::string bytes)
            : key(name), found(present), value(std::move(bytes)) {}
    };
    class batch_view {
    public:
        batch_entry* begin() const { return data_; }
        batch_entry* end() const { return data_ + size_; }
        size_t size() const { return size_; }
        batch_entry* find(const std::string& key) const {
            auto it = std::lower_bound(begin(), end(), key,
                [](const batch_entry& entry, const std::string& name) { return
                    entry.key < name; });
            return it == end() || it->key != key ? nullptr : it;
        }
    private:
        friend class db;
        batch_view(batch_entry* data, size_t size) : data_(data), size_(size) {}
        batch_entry* data_;
        size_t size_;
    };
    batch_status watch(std::vector<std::string> keys,
        std::vector<watch_token>& out);
    // Register before probing data. Notifications are readiness hints for any
    // committed change; the waiter must validate the current type/value.
    std::shared_ptr<key_wait_subscription> wait_for_keys(
        const std::vector<std::string>& keys);
    key_wait_token wait_for_zsets(const std::vector<std::string>& keys);
    key_wait_token wait_for_lists(const std::vector<std::string>& keys);

    using batch_callback = std::function<bool(batch_view)>;
    using page_visitor =
        std::function<bool(uint64_t, const std::string&)>;
    using page_callback = std::function<bool(page_transaction&)>;
    using page_copy_callback =
        std::function<bool(page_transaction&, page_transaction&)>;
    // Hold both DB/key locks in a total order. The source remains read-only;
    // destination metadata/pages commit atomically, with its own observers.
    batch_status copy_pages_to(const std::string& source, db& destination,
        const std::string& target, const page_copy_callback& callback);

    // Internal cross-DB primitive: prepare both engines before publishing
    // either. Caller enters without shared admission, like EXEC. Unsupported
    // engines decline before the callback. No sequential
    // write/compensating-delete fallback. Same-DB calls use one transaction.
    batch_status transact_pages_with(const std::string& source,
        db& destination, const std::string& target,
        const page_copy_callback& callback);

    // Locks logical keys exactly as transact does. Page reads are lazy and
    // owning; root/page changes commit together. Unsupported engines decline
    // before calling back. Callbacks cannot recursively access this DB.
    batch_status transact_pages(std::vector<std::string> keys,
        const page_callback& callback);
    // Read native pages under the same locks as legacy root transactions.
    // Page edits/resets are rejected; root writes use ordinary observers and
    // reclaim an overwritten native page family. Legacy roots may be large.
    batch_status transact_page_reads(std::vector<std::string> keys,
        const page_callback& callback);

    // Keys are deduplicated; entries are sorted by key. Lock stripes are
    // acquired in ascending order. The callback may yield, but may not recurse
    // into this DB or retain its view. false aborts all proposed changes.
    batch_status transact(std::vector<std::string> keys,
        const batch_callback& callback);
    using copy_callback = std::function<bool(bool, const std::string&, bool,
        std::string&, update_action&)>;
    // The callback may only propose a destination change; neither DB may be
    // recursively accessed. Both source and destination remain locked.
    batch_status copy_to(const std::string& source, db& destination, const
        std::string& target, const copy_callback& callback);
    // Must commit every action or leave every value unchanged on failure.
    // The default explicitly declines multi-key writes. Never emulate this by
    // sequential writes without rollback; engine implementations own atomicity.
protected:
    virtual batch_status dbapply(const std::vector<batch_entry>&) {
        return batch_status::unsupported;
    }
    virtual bool dbsupports_prepare() const { return false; }
    virtual bool dbsupports_owned_prepare() const { return false; }
    // Cold group replay owns frames until every DB is prepared. Inputs must
    // outlive the frame; destruction before publish rolls back under locks.
    virtual batch_status dbprepare_owned_pages(
        const std::vector<batch_entry>&, const std::vector<page_change>&,
        std::unique_ptr<prepared_batch>&) { return batch_status::unsupported; }
    virtual bool dbsupports_family_transfer(const db&) const { return false; }
    virtual batch_status dbprepare_pages(const std::vector<batch_entry>&,
        const std::vector<page_change>&, prepare_callback&) {
        return batch_status::unsupported;
    }
    // Retained MOVE storage/redo locks; raw hooks emit no logical events.
    virtual lookup_status dblookup_bounded_root(const std::string&,
        std::string&) { return lookup_status::error; }
    virtual bool dbsupports_page_stream() const { return false; }
    virtual batch_status dbvisit_pages(const std::string&,
        const page_visitor&) { return batch_status::unsupported; }
    virtual bool dbsupports_sync() const { return false; }
    virtual sync_status dbsync_key(const std::string&) {
        return sync_status::unsupported;
    }
    virtual bool dbsupports_pages() const { return false; }
    virtual bool dbatomic_workspace() const { return true; }
    virtual lookup_status dblookup_page(const std::string&, uint64_t,
        std::string&) { return lookup_status::error; }
    virtual batch_status dbapply_pages(const std::vector<batch_entry>&,
        const std::vector<page_change>&) { return batch_status::unsupported; }


public:
    // Serializes read/modify/write with every public get/set/del for this key.
    // The callback must not recursively access this database. It may yield;
    // the lock is fiber-aware and shared across worker threads.
    bool update(const std::string& key, const update_callback& callback);
    // Metadata writers may preserve the native page family under the same
    // key lock. Other writes retain normal whole-value replacement semantics.
    // The callback sets preserve_pages only for a valid paged metadata write.
    using root_update_callback =
        std::function<update_action(bool, std::string&, bool&)>;
    bool update_root(const std::string& key,
        const root_update_callback& callback);
    lookup_status lookup(const std::string& key, std::string& value);

    // Replication applies encoded bytes with the same key locks, but without
    // re-emitting observer events back into the replication stream.
    bool replica_set(const std::string& key, const std::string& value);
    bool replica_del(const std::string& key);
    bool replica_erase(const std::string& key);
    // Apply a complete received page transaction without re-emitting observers.
    // Root declarations and page addresses must be sorted and unique.
    // Ordered deltas for this DB. Validate every prefix under all key locks
    // and commit once. Replaces no whole in-memory page families to stage.
    // Primary ordered commit. Preflight every observer before the one engine
    // commit, then publish the original ordered deltas without losing history.
    batch_status commit_page_sequence(const std::vector<page_update>& updates);
    batch_status replica_apply_pages(const std::vector<page_update>& updates);
    batch_status replica_apply_pages(const std::vector<batch_entry>& roots,
        const std::vector<page_change>& pages);


protected:
    virtual bool dbinline_strings() const { return false; }
    virtual inline_status dbread_inline(const std::string&, int64_t,
        std::string&) { return inline_status::fallback; }
    static inline_status decode_inline(const std::string& root, int64_t now,
        std::string& out);
    // Legacy engines can override this to distinguish I/O failure from absence.
    virtual lookup_status dblookup(const std::string& key, std::string& value) {
        return dbget(key, value) ? lookup_status::found :
            lookup_status::missing;
    }

public:
    /**
     * @brief Open a database file.
     * 
     * @param path The path of the database file.
     * @param watchers A pointer to a db_watchers object to be notified of
         database changes.
     * @return true if the database is opened successfully, false otherwise.
     */
    bool open(const char* path, db_watchers* watchers = NULL,
              std::shared_ptr<const record_codec> codec = nullptr);

    bool open_observed(const char* path, std::shared_ptr<db_watchers> watchers,
            std::shared_ptr<const record_codec> codec = nullptr);

    // Exact native storage roots; no delimiter encoding or reparsing.
    // Unsupported engines decline before opening storage.
    bool open_paths(const std::vector<std::string>& paths,
        db_watchers* watchers = NULL,
        std::shared_ptr<const record_codec> codec = nullptr);
    bool open_observed_paths(const std::vector<std::string>& paths,
        std::shared_ptr<db_watchers> watchers,
        std::shared_ptr<const record_codec> codec = nullptr);

    // Only metadata is inspected. Call inside native MOVE prepare, then
    // publish once in complete, or destroy before releasing admission.
    bool prepare_move_observers(db& target, const family_move& move,
        std::unique_ptr<family_publication>& out);
    // Disk redo is physical only. Prepare local WATCH/expiry/readiness here,
    // then publish under the same pair locks after physical completion.
    bool prepare_move_effects(db& target, const family_move& move,
        std::unique_ptr<family_publication>& out);

    // Select an immutable codec at open time, before worker threads start.
    const record_codec& value_codec() const { return *codec_; }

    /**
     * @brief Set the value of a key.
     * 
     * @param key The key.
     * @param value The value.
     * @return true if the value is set successfully, false otherwise.
     */
    bool set(const std::string& key, const std::string& value);

    /**
     * @brief Get the value of a key.
     * 
     * @param key The key.
     * @param value The value.
     * @return true if the value is retrieved successfully, false otherwise.
     */
    bool get(const std::string& key, std::string& value);

    /**
     * @brief Delete a key.
     * 
     * @param key The key.
     * @return true if the key is deleted successfully, false otherwise.
     */
    bool del(const std::string& key);

    /**
     * @brief Create a cursor for iterating over the keys in the database.
     * 
     * @return A pointer to the cursor.
     */
    db_cursor* create_cursor();

    /**
     * @brief Scan the database using a cursor.
     * 
     * @param cursor The cursor.
     * @param keys The keys to be scanned.
     * @param max The maximum number of keys to be scanned.
     * @return true if the scan is successful, false otherwise.
     */
    bool scan(db_cursor& cursor, std::vector<std::string>& keys, size_t max);

protected:
    /**
     * @brief Open the database.
     * 
     * This method must be implemented by subclasses to provide
         database-specific implementation.
     * 
     * @param path The path of the database file.
     * @return true if the database is opened successfully, false otherwise.
     */
    virtual bool dbopen(const char* path) = 0;
    virtual bool dbopen_paths(const std::vector<std::string>& paths) {
        (void) paths; return false;
    }

    /**
     * @brief Set the value of a key.
     * 
     * This method must be implemented by subclasses to provide
         database-specific implementation.
     * 
     * @param key The key.
     * @param value The value.
     * @return true if the value is set successfully, false otherwise.
     */
    virtual bool dbset(const std::string& key, const std::string& value) = 0;

    /**
     * @brief Get the value of a key.
     * 
     * This method must be implemented by subclasses to provide
         database-specific implementation.
     * 
     * @param key The key.
     * @param value The value.
     * @return true if the value is retrieved successfully, false otherwise.
     */
    virtual bool dbget(const std::string& key, std::string& value) = 0;

    /**
     * @brief Delete a key.
     * 
     * This method must be implemented by subclasses to provide
         database-specific implementation.
     * 
     * @param key The key.
     * @return true if the key is deleted successfully, false otherwise.
     */
    virtual bool dbdel(const std::string& key) = 0;

    /**
     * @brief Create a cursor for iterating over the keys in the database.
     * 
     * This method must be implemented by subclasses to provide
         database-specific implementation.
     * 
     * @return A pointer to the cursor.
     */
    virtual db_cursor* dbcreate_cursor() = 0;

    /**
     * @brief Get the size of the database.
     *
     * This method must be implemented by subclasses to provide
         database-specific implementation.
     * 
     * @return The size of the database.
     */
    virtual size_t dbsize() const = 0;

    /**
     * @brief Scan the database using a cursor.
     * 
     * This method must be implemented by subclasses to provide
         database-specific implementation.
     * 
     * @param size The size of the database.
     * @param cursor The cursor.
     * @param keys The keys to be scanned.
     * @param max The maximum number of keys to be scanned.
     * @return true if the scan is successful, false otherwise.
     */
    virtual bool dbscan(size_t size, db_cursor& cursor,
                std::vector<std::string>& keys, size_t max) = 0;

public:
    /**
     * @brief Get the type of the database.
     * 
     * This method must be implemented by subclasses to provide
         database-specific implementation.
     * 
     * @return The type of the database.
     */
    NODISCARD virtual const char* get_dbtype() const = 0;

private:
    struct move_effects;
    friend class move_recovery;
    friend class move_access;
    friend class move_storage;
    friend class move_memory;
    friend class replica_group;
    friend class family_plan;
    friend class family_workspace;
    friend class page_transaction;
    friend class page_workspace;
    batch_status transact_pages_impl(std::vector<std::string> keys,
        const page_callback& callback, bool observe,
        const page_update* ordered = nullptr, size_t count = 0);

    // Trusted actor-local delta. The workspace validated every staged command
    // and retains exclusive admission, so source roots need not be read again.
    batch_status commit_workspace(std::vector<batch_entry>& roots,
        std::vector<page_change>& pages,
        const std::vector<page_update>& observations);
    bool can_observe_page_sequence(const std::vector<page_update>& updates);
    void observe_page_sequence(const std::vector<page_update>& updates);

    batch_status commit_pages(std::vector<batch_entry>& roots,
        std::vector<page_change>& pages, bool observe,
        const page_update* ordered = nullptr, size_t count = 0);
    std::vector<key_ready_change> prepare_ready(
        const std::vector<batch_entry>& roots,
        const page_update* ordered = nullptr, size_t count = 0,
        const std::vector<page_change>* pages = nullptr);
    key_ready_change ready_change(const std::string& key,
        const std::string* value);
    bool apply_root(const std::string& key, const std::string* value);

    void invalidate_watch(const std::string& key, bool erasing = false);

    std::unique_ptr<expiry_index> expiry_;
    void track_expiry(const std::string& key,
        const std::string* value) noexcept;
    std::shared_ptr<const record_codec> codec_;
    struct paired_commit;
    acl::fiber_mutex& key_mutex(const std::string& key);
    batch_status commit_page_pair(std::vector<batch_entry>& roots,
        std::vector<page_change>& pages, db& destination,
        std::vector<batch_entry>& target_roots,
        std::vector<page_change>& target_pages);
    void publish_page_effects(const std::vector<batch_entry>& roots,
        const std::vector<page_change>& pages,
        const std::vector<key_ready_change>& ready);
    void observe_pages(const std::vector<batch_entry>& roots,
        const std::vector<page_change>& pages);
    struct lock_table;
    std::unique_ptr<lock_table> locks_;
    std::shared_ptr<watch_registry> watch_registry_;
    std::shared_ptr<key_wait_registry> wait_registry_;
    std::shared_ptr<key_wait_registry> ready_registry_;
    db_watchers* watchers_ = nullptr;
    std::shared_ptr<db_watchers> watchers_owner_;
    bool opened_ = false;

public:
    /**
     * @brief Create a database with rocksdb.
     * 
     * @return A shared pointer to the database.
     */
    static shared_db create_rdb();

    /**
     * @brief Create a database with wiredtiger.
     * 
     * @return A shared pointer to the database.
     */
    static shared_db create_wdb();

    /**
     * @brief Create a memory-mapped database.
     * 
     * @return A shared pointer to the memory-mapped database.
     */
    static shared_db create_mdb(size_t count);

    /**
     * @brief Create a memory-mapped database with hash table.
     * 
     * @return A shared pointer to the memory-mapped database with hash table.
     */
    static shared_db create_mdb_htable(size_t count);

    /**
     * @brief Create a memory-mapped database with AVL tree.
     * 
     * @return A shared pointer to the memory-mapped database with AVL tree.
     */
    static shared_db create_mdb_avl();

    /**
     * @brief Create a memory-mapped database with TBB tree.
     * 
     * @return A shared pointer to the memory-mapped database with TBB tree.
     */
    static shared_db create_mdb_tbb(size_t count);
protected:
    // Appended cold hooks preserve the order of existing virtual methods.
    virtual bool dbsupports_detached_pages() const { return false; }
    virtual batch_status dbdetach_pages(const std::string&,
        std::unique_ptr<prepared_pages>&) { return batch_status::unsupported; }
    virtual bool dbclose_for_exit() { return true; }
};

} // namespace pkv
