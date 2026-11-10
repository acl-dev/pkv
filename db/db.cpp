#include "stdafx.h"
#include "common/key_wait.h"
#include "common/redis_bitmap.h"
#include "common/redis_string_util.h"
#include <array>
#include "coder/record_codec.h"
#include "coder/map_page.h"
#include "coder/list_page.h"
#include "coder/string_page.h"

#ifdef HAS_ROCKSDB
#include "rocksdb/rdb.h"
#endif

#ifdef HAS_WT
#include "wt/wdb.h"
#endif

#include "mdb/mdb.h"
#include "mdb/mdb_htable.h"
#include "mdb/mdb_avl.h"
#ifdef HAS_TBB
#include "mdb/mdb_tbb.h"
#endif

#include "db_watcher.h"
#include "expiry_index.h"
#include "db_cursor.h"
#include "db.h"
#include "page_transaction.h"
#include "root_workspace.h"
#include "map_index_workspace.h"

namespace pkv {
#ifdef PKV_PACKED_RESP_ADOPTION
db::batch_status packed_resp_adopt(db&,const db::batch_entry&,
    std::vector<page_change>&);
#endif
const size_t db::page_inspection::max_bytes;


struct db::lock_table {
    const size_t mask;
    std::unique_ptr<acl::fiber_mutex[]> stripes;
    explicit lock_table(bool local) : mask(local ? 0 : 255),
        stripes(new acl::fiber_mutex[mask + 1]) {}
    size_t index(const std::string& key) const {
        return std::hash<std::string>()(key) & mask;
    }
    acl::fiber_mutex& for_key(const std::string& key) { return
        stripes[index(key)]; }

    bool lock_keys(const std::string* keys, size_t count,
        std::vector<std::unique_lock<acl::fiber_mutex>>& guards) {
        std::array<uint64_t, 4> selected{};
        for (size_t i = 0; i < count; ++i) {
            const auto stripe = index(keys[i]);
            selected[stripe / 64] |= uint64_t{1} << (stripe % 64);
        }
        size_t distinct = 0;
        for (auto bits : selected) {
#if defined(__GNUC__) || defined(__clang__)
            distinct += __builtin_popcountll(bits);
#else
            while (bits) { bits &= bits - 1; ++distinct; }
#endif
        }
        guards.reserve(distinct);
        // Visit stripes in increasing order, as in the two-key path.
        for (size_t word = 0; word < selected.size(); ++word) {
            auto bits = selected[word];
            while (bits) {
#if defined(__GNUC__) || defined(__clang__)
                const auto bit = __builtin_ctzll(bits);
#else
                size_t bit = 0;
                for (auto low = bits; !(low & 1); low >>= 1) ++bit;
#endif
                auto& mutex = stripes[word * 64 + bit];
                if (!mutex.lock()) return false;
                guards.emplace_back(mutex, std::adopt_lock);
                bits &= bits - 1;
            }
        }
        return true;
    }
};

acl::fiber_mutex& db::key_mutex(const std::string& key) {
    return locks_->for_key(key);
}

bool db::supports_compact_zsets() const {
#ifdef PKV_COMPACT_ZSET
    return dbinline_strings();
#else
    return false;
#endif
}

bool db::supports_compact_set_leaves() const {
#ifdef PKV_COMPACT_SET_LEAF
    return dbinline_strings() && std::string().capacity() >= 22;
#else
    return false;
#endif
}

bool db::supports_compact_set_fragments() const {
#ifdef PKV_COMPACT_SET_FRAGMENTS
    return dbinline_strings();
#else
    return false;
#endif
}

bool db::supports_compact_strings() const {
    return dbinline_strings() && (!watchers_ || !watchers_->requires_page_payload());
}

db::db() : db(false) {}
db::db(bool actor_local) : codec_(default_record_codec()),
    locks_(new lock_table(actor_local)),
    watch_registry_(actor_local ? nullptr :
        std::make_shared<watch_registry>()),
    wait_registry_(actor_local ? nullptr :
        std::make_shared<key_wait_registry>()),
    ready_registry_(actor_local ? nullptr :
        std::make_shared<key_wait_registry>(true)) {}
db::~db() = default;

class dummy_db : public db {
public:
    dummy_db() = default;
    ~dummy_db() override = default;

    bool dbopen(const char*) override {
        return false;
    }

    bool dbset(const std::string&, const std::string&) override {
        return false;
    }

    bool dbget(const std::string&, std::string&) override {
        return false;
    }

    bool dbdel(const std::string&) override {
        return false;
    }

    db_cursor* dbcreate_cursor() override {
        return nullptr;
    }

    bool dbscan(size_t, db_cursor&, std::vector<std::string>&, size_t) override
        {
        return false;
    }

    NODISCARD const char* get_dbtype() const override {
        return "dummy_db";
    }

    NODISCARD size_t dbsize() const override {
        return 0;
    }
};

shared_db db::create_rdb() {
#ifdef HAS_ROCKSDB
    return std::make_shared<rdb>();
#else
    return std::make_shared<dummy_db>();
#endif
}

shared_db db::create_wdb() {
#ifdef HAS_WT
    return std::make_shared<wdb>();
#else
    return std::make_shared<dummy_db>();
#endif
}

shared_db db::create_mdb(size_t count) {
    return std::make_shared<mdb>(count);
}

shared_db db::create_mdb_htable(size_t count) {
    return std::make_shared<mdb_htable>(count);
}

shared_db db::create_mdb_avl() {
    return std::make_shared<mdb_avl>();
}

shared_db db::create_mdb_tbb(size_t count) {
#ifdef HAS_TBB
    return std::make_shared<mdb_tbb>(count);
#else
    (void) count;
    return std::make_shared<dummy_db>();
#endif
}

bool db::open(const char *path, db_watchers* watchers, std::shared_ptr<const
    record_codec> codec) {
    if (codec) codec_ = std::move(codec);
    watchers_owner_.reset();
    watchers_ = watchers;
    const bool ok = this->dbopen(path);
    if (ok) opened_ = true;
    return ok;
}

bool db::open_paths(const std::vector<std::string>& paths,
    db_watchers* watchers, std::shared_ptr<const record_codec> codec) {
    if (codec) codec_ = std::move(codec);
    watchers_owner_.reset();
    watchers_ = watchers;
    const bool ok = dbopen_paths(paths);
    if (ok) opened_ = true;
    return ok;
}

bool db::open_observed_paths(const std::vector<std::string>& paths,
    std::shared_ptr<db_watchers> watchers,
    std::shared_ptr<const record_codec> codec) {
    if (!open_paths(paths, watchers.get(), std::move(codec))) return false;
    watchers_owner_ = std::move(watchers);
    return true;
}

bool db::replica_erase(const std::string& key) {
    execution_guard admission;
    auto& mutex = locks_->for_key(key);
    if (!mutex.lock()) {
        return false;
    }
    std::unique_lock<acl::fiber_mutex> guard(mutex, std::adopt_lock);
    std::string previous;
    const auto status = dblookup(key, previous);
    if (status == lookup_status::error) {
        return false;
    }
    if (status == lookup_status::missing) return true;
    const bool ok = apply_root(key, nullptr);
    if (ok) { track_expiry(key, nullptr); invalidate_watch(key, true); }
    return ok;
}

bool db::open_observed(const char* path, std::shared_ptr<db_watchers> watchers,
        std::shared_ptr<const record_codec> codec) {
    if (!open(path, watchers.get(), std::move(codec))) {
        return false;
    }
    watchers_owner_ = std::move(watchers);
    return true;
}

bool db::set(const std::string &key, const std::string &value) {
    execution_guard admission;
    if (execution_replica_read()) return true;
    auto& mutex = locks_->for_key(key);
    if (!mutex.lock()) return false;
    std::unique_lock<acl::fiber_mutex> lock(mutex, std::adopt_lock);
    bool ret = apply_root(key, &value);
    if (ret) { track_expiry(key, &value); invalidate_watch(key); }
    if (watchers_) {
        watchers_->on_set(key, value, ret);
    }
    return ret;
}

db::lookup_status db::lookup(const std::string& key, std::string& value) {
    execution_guard admission;
    auto& mutex = locks_->for_key(key);
    if (!mutex.lock()) return lookup_status::error;
    std::unique_lock<acl::fiber_mutex> lock(mutex, std::adopt_lock);
    auto status = dblookup(key, value);
    if (watchers_) watchers_->on_get(key, value, status ==
        lookup_status::found);
    return status;
}

bool db::get(const std::string& key, std::string& value) {
    return lookup(key, value) == lookup_status::found;
}

db::inline_status db::decode_inline(const std::string& root,
    std::string& out, size_t offset, size_t limit) {
    string_root_header header;
    if (string_page_codec::decode_header(root, header) !=
        string_page_status::ok || header.pages ||
        (header.expires_at_ms >= 0 && header.expires_at_ms <= unix_time_ms()))
        return inline_status::fallback;
    if (offset < header.length) {
        const auto count = std::min(limit, static_cast<size_t>(header.length) - offset);
        out.assign(root.data() + header.payload_offset + offset, count);
    }
    return inline_status::found;
}

db::inline_status db::read_inline_string(const std::string& key,
    std::string& out, size_t offset, size_t limit) {
    out.clear();
    if (!dbinline_strings()) return inline_status::fallback;
    execution_guard admission;
    const auto hash = std::hash<std::string>()(key);
    auto& mutex = locks_->stripes[hash & locks_->mask];
    const bool immediate = mutex.trylock();
    if (!immediate && !mutex.lock()) return inline_status::error;
    std::unique_lock<acl::fiber_mutex> lock(mutex, std::adopt_lock);
    try {
        // Share the hash only when acquiring the logical lock did not yield.
        // Subclasses retain their virtual hooks, and waited acquisitions keep
        // the original lookup path if a caller's key changed while suspended.
        if (immediate && typeid(*this) == typeid(mdb)) {
            return static_cast<mdb*>(this)->read_inline_hashed(key, out,
                offset, limit, hash);
        }
        return dbread_inline(key, out, offset, limit);
    } catch (const std::bad_alloc&) {
        out.clear(); return inline_status::error;
    }
}

struct db::native_string_view {
    db& database;
    const std::string& key;
    const std::string& root;
    const string_root_header& header;
    page_reader reader;

    template<class Consume>
    bool visit(uint64_t offset, size_t count, const Consume& consume) {
        if (!header.pages) {
            consume(root.data() + header.payload_offset + offset, offset, count);
            return true;
        }
        if (!reader.read_ && !database.dbbind_page_reader(key, reader)) return false;
        constexpr auto page_bytes = string_page_codec::page_bytes;
        while (count) {
            const auto position = static_cast<size_t>(offset % page_bytes);
            const auto expected = std::min<uint64_t>(page_bytes,
                header.length - (offset - position));
            const auto* page = reader.read_(reader.context_, offset / page_bytes + 1);
            if (!page || page->size() != expected) return false;
            const auto bytes = std::min(count, static_cast<size_t>(expected) - position);
            if (!consume(page->data() + position, offset, bytes)) break;
            offset += bytes;
            count -= bytes;
        }
        return true;
    }
};

template<class Read>
db::inline_status db::read_native_string(const std::string& key, const Read& read) {
    const auto& backend = typeid(*this);
    const bool native_mdb = backend == typeid(mdb);
    if (!native_mdb
#ifdef HAS_TBB
        && backend != typeid(mdb_tbb)
#endif
    ) return inline_status::fallback;
    try {
        const std::string snapshot(key);
        execution_guard admission;
        const auto hash = std::hash<std::string>()(snapshot);
        auto& mutex = locks_->stripes[hash & locks_->mask];
        if (!mutex.trylock() && !mutex.lock()) return inline_status::error;
        std::unique_lock<acl::fiber_mutex> guard(mutex, std::adopt_lock);
        const std::string* root = nullptr;
        page_reader pages;
        if (native_mdb) root = static_cast<mdb*>(this)->borrow_root(snapshot, hash);
#ifdef HAS_TBB
        else root = static_cast<mdb_tbb*>(this)->borrow_root(snapshot, pages);
#endif
        if (!root) return inline_status::missing;
        string_root_header header;
        if (string_page_codec::decode_header(*root, header) != string_page_status::ok ||
            (header.expires_at_ms >= 0 && header.expires_at_ms <= unix_time_ms()))
            return inline_status::fallback;
        // The logical key lock pins the root and each borrowed page until
        // the consumer returns. Physical shard locks are free during scans.
        native_string_view view{*this, snapshot, *root, header, pages};
        return read(view) ? inline_status::found : inline_status::fallback;
    } catch (const std::bad_alloc&) { return inline_status::error; }
}

db::inline_status db::read_string_range(const std::string& key, long long start,
    long long end, std::string& out) {
    out.clear();
    const auto status = read_native_string(key, [&](native_string_view& view) {
        size_t offset, count;
        if (!string_slice(static_cast<size_t>(view.header.length), start, end, offset, count))
            return true;
        if (view.header.pages) out.reserve(count);
        return view.visit(offset, count, [&](const char* data, uint64_t, size_t size) {
            out.append(data, size);
            return true;
        });
    });
    if (status != inline_status::found) out.clear();
    return status;
}

db::inline_status db::read_string_bit(const std::string& key, uint64_t offset,
    long long& bit) {
    bit = 0;
    const auto byte = offset / 8;
    return read_native_string(key, [&](native_string_view& view) {
        if (byte >= view.header.length) return true;
        return view.visit(byte, 1, [&](const char* data, uint64_t, size_t) {
            bit = (static_cast<unsigned char>(*data) >> (7 - offset % 8)) & 1;
            return true;
        });
    });
}

db::inline_status db::scan_string_bitmap(const std::string& key, const bitmap_range& range,
    int bit, long long& out) {
    long long result = bit < 0 ? 0 : -1;
    const auto status = read_native_string(key, [&](native_string_view& view) {
        uint64_t first, last;
        if (!bitmap_bounds(view.header.length, range, bit < 0, first, last)) return true;
        const auto start = first / 8;
        if (!view.visit(start, last / 8 - start + 1,
                [&](const char* data, uint64_t offset, size_t count) {
                    return !bitmap_scan_chunk(data, offset, count, first, last, bit, result);
                })) return false;
        if (result < 0 && bit == 0 && !range.end_given)
            result = static_cast<long long>(view.header.length * 8);
        return true;
    });
    if (status == inline_status::found) out = result;
    return status;
}

struct db::string_edit {
    const char* value;
    string_byte_editor editor;
    void* context;

    bool apply(char* first, size_t count, char* second, size_t remaining) const {
        const char* bytes = value;
        char scratch[string_edit_bytes];
        if (editor) {
            std::memcpy(scratch, first, count);
            if (remaining) std::memcpy(scratch + count, second, remaining);
            if (!editor(context, scratch)) return false;
            bytes = scratch;
        }
        std::memcpy(first, bytes, count);
        if (remaining) std::memcpy(second, bytes + count, remaining);
        return true;
    }
};

namespace {
template<class Edit, class FindPages>
bool edit_existing_string(std::string& root, size_t offset, size_t size,
    string_root_header& header, const Edit& edit, bool& changed, FindPages find_pages) {
    if (string_page_codec::decode_header(root, header) != string_page_status::ok ||
        offset > header.length || size > header.length - offset ||
        (header.expires_at_ms >= 0 && header.expires_at_ms <= unix_time_ms()))
        return false;
    if (!header.pages) {
        // Let the regular writer migrate the original inline root encoding.
        if (root[3] == 1) return false;
        changed = edit.apply(&root[header.payload_offset + offset], size, nullptr, 0);
        return true;
    }

    auto* pages = find_pages();
    if (!pages) return false;
    constexpr auto page_bytes = string_page_codec::page_bytes;
    const auto index = offset / page_bytes;
    const auto position = offset % page_bytes;
    const auto expected = std::min<uint64_t>(page_bytes, header.length - index * page_bytes);
    auto* first = pages->get(index + 1);
    if (!first || first->size() != expected) return false;
    const auto count = std::min(size, static_cast<size_t>(expected) - position);
    const auto remaining = size - count;
    char* tail = nullptr;
    if (remaining) {
        auto* second = pages->get(index + 2);
        const auto next_size = std::min<uint64_t>(page_bytes,
            header.length - (index + 1) * page_bytes);
        if (!second || second->size() != next_size) return false;
        tail = &(*second)[0];
    }
    // Both pages are validated before the editor can publish any bytes.
    changed = edit.apply(&(*first)[position], count, tail, remaining);
    return true;
}
} // namespace

db::inline_status db::overwrite_string_range(const std::string& key,
    uint64_t offset, const std::string& value, long long& length) {
    if (value.empty() || value.size() > string_page_codec::page_bytes ||
        offset > string_page_codec::write_bytes - value.size()) return inline_status::fallback;
    return edit_string_range(key, offset, value.size(), {value.data(), nullptr, nullptr}, &length);
}

db::inline_status db::edit_string_bytes(const std::string& key, uint64_t offset,
    size_t count, string_byte_editor edit, void* context) {
    if (!edit || !count || count > string_edit_bytes ||
        offset > string_page_codec::max_bytes - count) return inline_status::fallback;
    return edit_string_range(key, offset, count, {nullptr, edit, context}, nullptr);
}

db::inline_status db::edit_string_range(const std::string& key, uint64_t offset,
    size_t count, const string_edit& edit, long long* length) {
    const auto& backend = typeid(*this);
    const bool native_mdb = backend == typeid(mdb);
    if (!native_mdb
#ifdef HAS_TBB
        && backend != typeid(mdb_tbb)
#endif
    ) return inline_status::fallback;
    try {
        const std::string snapshot(key);
        execution_guard admission;
        if (execution_replica_read() || (ready_registry_ && ready_registry_->subscribers()) ||
            (watchers_ && !watchers_->can_overwrite_string(snapshot.size())))
            return inline_status::fallback;
        auto& mutex = locks_->for_key(snapshot);
        if (!mutex.trylock() && !mutex.lock()) return inline_status::error;
        std::unique_lock<acl::fiber_mutex> guard(mutex, std::adopt_lock);
        string_root_header header;
        bool found = false, changed = false;
        if (native_mdb) {
            auto& native = *static_cast<mdb*>(this);
            const auto position = native.locate(snapshot);
            std::lock_guard<lock_t> shard(*native.locks_[position.shard]);
            auto* roots = native.find_store(position, false);
            if (!roots) return inline_status::fallback;
            const auto root = roots->find(snapshot);
            if (root == roots->end()) return inline_status::fallback;
            const auto pages = [&]() -> mdb::page_map* {
                auto* family = native.find_page_family(position.shard, false);
                if (!family) return nullptr;
                const auto owner = family->find(snapshot);
                return owner == family->end() ? nullptr : &owner->second;
            };
            found = edit_existing_string(root->second.value, static_cast<size_t>(offset),
                count, header, edit, changed, pages);
        }
#ifdef HAS_TBB
        else if (auto* record = static_cast<mdb_tbb*>(this)->find(snapshot)) {
            found = edit_existing_string(record->root, static_cast<size_t>(offset),
                count, header, edit, changed, [&] { return &record->pages; });
        }
#endif
        if (!found) return inline_status::fallback;
        if (changed) {
            track_deadline(snapshot, header.expires_at_ms);
            invalidate_watch(snapshot);
        }
        if (length) *length = static_cast<long long>(header.length);
        return inline_status::found;
    } catch (const std::bad_alloc&) { return inline_status::error; }
}

bool db::del(const std::string& key) {
    execution_guard admission;
    if (execution_replica_read()) return true;
    auto& mutex = locks_->for_key(key);
    if (!mutex.lock()) return false;
    std::unique_lock<acl::fiber_mutex> lock(mutex, std::adopt_lock);
    bool ret = apply_root(key, nullptr);
    if (ret) { track_expiry(key, nullptr); invalidate_watch(key, true); }
    if (watchers_) watchers_->on_del(key, ret);
    return ret;
}

bool db::replica_set(const std::string& key, const std::string& value) {
    execution_guard admission;
    auto& mutex = locks_->for_key(key);
    if (!mutex.lock()) return false;
    std::unique_lock<acl::fiber_mutex> lock(mutex, std::adopt_lock);
    const bool ok = apply_root(key, &value);
    if (ok) { track_expiry(key, &value); invalidate_watch(key); }
    return ok;
}

bool db::replica_del(const std::string& key) {
    execution_guard admission;
    auto& mutex = locks_->for_key(key);
    if (!mutex.lock()) return false;
    std::unique_lock<acl::fiber_mutex> lock(mutex, std::adopt_lock);
    std::string value;
    const auto status = dblookup(key, value);
    if (status == lookup_status::error) return false;
    if (status == lookup_status::missing) return true;
    const bool ok = apply_root(key, nullptr);
    if (ok) { track_expiry(key, nullptr); invalidate_watch(key, true); }
    return ok;
}

bool db::update(const std::string& key, const update_callback& callback) {
    return update_root(key, [&](bool found, std::string& bytes, bool&) {
        return callback(found, bytes);
    });
}

bool db::allows_page_writes() const {
    return dbsupports_pages() && (!watchers_ || watchers_->supports_pages());
}

bool db::update_root(const std::string& key,
    const root_update_callback& callback, const long long* deadline) {
    execution_guard admission;
    auto& mutex = locks_->for_key(key);
    if (!mutex.lock()) return false;
    std::unique_lock<acl::fiber_mutex> lock(mutex, std::adopt_lock);
    detail::root_workspace workspace(deadline ? 1 : 0);
    std::string fallback;
    auto& value = workspace.borrow_buffer(fallback);
    auto status = dblookup(key, value);
    if (status == lookup_status::error) return false;
    if (watchers_) watchers_->on_get(key, value, status ==
        lookup_status::found);
    bool preserve_pages = false;
    const auto action = callback(status == lookup_status::found, value,
        preserve_pages);
    if (execution_replica_read()) return true;
    switch (action) {
    case update_action::keep:
        return true;
    case update_action::write: {
        if (preserve_pages) {
            if (!dbsupports_pages() || status != lookup_status::found ||
                value.size() > page_transaction::max_value_bytes) return false;
            try {
                workspace.acquire();
                auto& roots = workspace.roots();
                roots.emplace_back(key, true, std::move(value));
                roots.front().action = update_action::write;
                return commit_pages(roots, workspace.pages(), true, nullptr, 0,
                    nullptr, nullptr, deadline) == batch_status::ok;
            } catch (const std::bad_alloc&) {
                return false;
            }
        }
        bool ok = apply_root(key, &value);
        if (ok) { track_expiry(key, &value); invalidate_watch(key); }
        if (watchers_) watchers_->on_set(key, value, ok);
        return ok;
    }
    case update_action::erase: {
        if (status == lookup_status::missing) return true;
        bool ok = apply_root(key, nullptr);
        if (ok) {
            track_expiry(key, nullptr); invalidate_watch(key, true);
        }
        if (watchers_) watchers_->on_del(key, ok);
        return ok;
    }
    }
    return false;
}

db::batch_status db::transact(std::vector<std::string> keys, const
    batch_callback& callback) {
    execution_guard admission;
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    if (keys.empty()) return batch_status::aborted;
    std::vector<std::unique_lock<acl::fiber_mutex>> guards;
    if (!locks_->lock_keys(keys.data(), keys.size(), guards))
        return batch_status::error;
    std::vector<batch_entry> entries;
    entries.reserve(keys.size());
    for (const auto& key : keys) {
        std::string value;
        const auto status = dblookup(key, value);
        if (status == lookup_status::error) return batch_status::error;
        if (watchers_) watchers_->on_get(key, value, status ==
            lookup_status::found);
        entries.emplace_back(key, status == lookup_status::found,
            std::move(value));
    }
    if (!callback(batch_view(entries.data(), entries.size()))) return
        batch_status::aborted;
    if (execution_replica_read()) return batch_status::ok;
    size_t changes = 0;
    batch_entry* single = nullptr;
    for (auto& entry : entries) {
        if (entry.action == update_action::keep || (entry.action ==
            update_action::erase && !entry.found)) continue;
        ++changes;
        single = &entry;
    }
    const auto ready = changes ? prepare_ready(entries) :
        std::vector<key_ready_change>();
    if (!ready.empty()) ready_registry_->prepare(ready.data(), ready.size());
    batch_status status = batch_status::ok;
    if (changes == 1) {
        const bool ok = single->action == update_action::write ?
            dbset(single->key, single->value) : dbdel(single->key);
        status = ok ? batch_status::ok : batch_status::error;
    } else if (changes > 1) {
        status = dbapply(entries);
    }
    if (status != batch_status::ok) return status;
    if (!ready.empty()) ready_registry_->publish(ready);
    for (const auto& entry : entries) {
        if (entry.action != update_action::keep) {
            track_expiry(entry.key, entry.action == update_action::erase ?
                nullptr : &entry.value);
            invalidate_watch(entry.key,
                entry.action == update_action::erase);
        }
    }
    if (watchers_) {
        for (const auto& entry : entries) {
            if (entry.action == update_action::write)
                watchers_->on_set(entry.key, entry.value, true);
            else if (entry.action == update_action::erase && entry.found)
                watchers_->on_del(entry.key, true);
        }
    }
    return batch_status::ok;
}

db::batch_status db::transact_pages(std::vector<std::string> keys,
    const page_callback& callback) {
    return transact_pages_impl(std::move(keys), callback, true);
}

db::batch_status db::transact_page_reads(std::vector<std::string> keys,
    const page_callback& callback) {
    bool storage_error = false;
    const auto status = transact(std::move(keys), [&](batch_view roots) {
        page_transaction tx(*this, roots);
        const bool accepted = callback(tx);
        storage_error = tx.storage_error_;
        if (!accepted || tx.failed_ || tx.has_changes()) return false;
        for (const auto& root : roots) if (root.clear_pages) return false;
        return true;
    });
    return storage_error ? batch_status::error : status;
}

db::batch_status db::transact_page(const std::string& key,
    const page_callback& callback) {
    return transact_single_page(key, callback, nullptr);
}

db::page_lock_scope::page_lock_scope(db& owner, const std::string& key)
: owner_(owner), key_(key) {}

bool db::page_lock_scope::can_edit_bound_list() const {
    return cache_collections_ && reader_available_ &&
        (!owner_.watchers_ || !owner_.watchers_->requires_page_payload());
}

db::page_lock_scope::~page_lock_scope() {
    if (mutex_) mutex_->unlock();
}

db::batch_status db::page_lock_scope::transact(db& owner,
    const std::string& key, const page_callback& callback,
    const page_root_callback* root, bool skip_missing) {
    return transact(owner, key, callback, root, skip_missing, nullptr);
}

db::batch_status db::page_lock_scope::transact(db& owner,
    const std::string& key, const page_callback& callback,
    const page_root_callback* root, bool skip_missing,
    const long long* deadline) {
    return transact(owner, key, callback, root, skip_missing, deadline, nullptr);
}

db::batch_status db::page_lock_scope::transact(db& owner,
    const std::string& key, const page_callback& callback,
    const page_root_callback* root, bool skip_missing,
    const long long* deadline, const list_page_edit* edit) {
    if (&owner != &owner_ || key != key_) return batch_status::error;
    if (!owner_.dbinline_strings())
        return owner_.transact_single_page(key, callback, root, true,
            skip_missing, deadline);
    if (!mutex_) {
        if (!owner_.dbsupports_pages()) return batch_status::unsupported;
        if (!lock_key()) return batch_status::error;
    }
    if (skip_missing && root_available_ && !root_found_) return batch_status::ok;
    return owner_.transact_page_keys(&key_, 1, callback, true, nullptr, 0,
        root, true, skip_missing, this, deadline, edit);
}

bool db::page_lock_scope::lock_key() {
    if (mutex_) return true;
    auto& mutex = owner_.key_mutex(key_);
    if (!mutex.lock()) return false;
    mutex_ = &mutex;
    cache_root_ = typeid(owner_) == typeid(mdb);
    cache_collections_ = cache_root_;
#ifdef HAS_TBB
    cache_collections_ = cache_collections_ || typeid(owner_) == typeid(mdb_tbb);
#endif
    return true;
}

db::inline_status db::page_lock_scope::read_inline_string(db& owner,
    const std::string& key, std::string& out) {
    out.clear();
    if (&owner != &owner_ || key != key_) return inline_status::error;
    if (!owner_.dbinline_strings()) return inline_status::fallback;
    if (!lock_key()) return inline_status::error;
    try {
#ifdef HAS_TBB
        if (cache_collections_ && !cache_root_) {
            auto& backend = static_cast<mdb_tbb&>(owner_);
            page_reader reader;
            if (reader_available_) reader = reader_;
            const auto* stored = backend.borrow_root(key_, reader);
            if (!stored) return inline_status::missing;
            reader_ = reader;
            reader_available_ = true;
            return decode_inline(*stored, out, 0, std::string::npos);
        }
#endif
        return owner_.dbread_inline(key_, out, 0, std::string::npos);
    } catch (const std::bad_alloc&) {
        out.clear();
        return inline_status::error;
    }
}

db::batch_status db::page_lock_scope::replace_root(db& owner,
    const std::string& key, const page_root_callback& root) {
    if (&owner != &owner_ || key != key_) return batch_status::error;
    if (!owner_.dbinline_strings()) return owner_.replace_page_root(key, root);
    if (!mutex_) {
        if (!owner_.dbsupports_pages()) return batch_status::unsupported;
        if (!lock_key()) return batch_status::error;
    }
    return owner_.transact_page_keys(&key_, 1, {}, true, nullptr, 0,
        &root, false, false, this);
}

db::batch_status db::transact_existing_page(const std::string& key,
    const page_callback& callback) {
    return transact_single_page(key, callback, nullptr, true, true);
}

db::batch_status db::transact_page(const std::string& key,
    const page_callback& callback, const page_root_callback& root) {
    return transact_single_page(key, callback, &root);
}

db::batch_status db::transact_page(const std::string& key,
    const page_callback& callback, const page_root_callback* root,
    const long long* deadline) {
    return transact_single_page(key, callback, root, true, false, deadline);
}

db::batch_status db::replace_page_root(const std::string& key,
    const page_root_callback& root) {
    return transact_single_page(key, {}, &root, false);
}

db::batch_status db::transact_single_page(const std::string& key,
    const page_callback& callback, const page_root_callback* root,
    bool read_value, bool skip_missing, const long long* deadline) {
    try {
        // Detach before admission/lock waits or virtual backend hooks yield.
        const std::string snapshot(key);
        execution_guard admission;
        if (!dbsupports_pages()) return batch_status::unsupported;
        return transact_page_keys(&snapshot, 1, callback, true, nullptr, 0,
            root, read_value, skip_missing, nullptr, deadline);
    } catch (const std::bad_alloc&) {
        return batch_status::error;
    }
}

db::batch_status db::transact_pages_impl(std::vector<std::string> keys,
    const page_callback& callback, bool observe,
    const page_update* ordered, size_t count) {
    execution_guard admission;
    if (!dbsupports_pages()) return batch_status::unsupported;
    try {
        std::sort(keys.begin(), keys.end());
        keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
        return transact_page_keys(keys.data(), keys.size(), callback,
            observe, ordered, count);
    } catch (const std::bad_alloc&) {
        return batch_status::error;
    }
}

db::batch_status db::transact_page_keys(const std::string* keys,
    size_t key_count, const page_callback& callback, bool observe,
    const page_update* ordered, size_t count, const page_root_callback* root,
    bool read_value, bool skip_missing, page_lock_scope* scope,
    const long long* deadline, const list_page_edit* edit) {
    try {
        if (!key_count) {
            return batch_status::aborted;
        }
        std::unique_lock<acl::fiber_mutex> first_guard, second_guard;
        std::vector<std::unique_lock<acl::fiber_mutex>> guards;
        if (scope) {
            assert(key_count == 1);
        } else if (key_count == 1) {
            // Keep the single-owner lock on the stack, including rollback.
            auto& mutex = locks_->for_key(*keys);
            if (!mutex.lock()) {
                return batch_status::error;
            }
            first_guard = std::unique_lock<acl::fiber_mutex>(mutex,
                std::adopt_lock);
        } else if (key_count == 2) {
            const auto left = locks_->index(keys[0]);
            const auto right = locks_->index(keys[1]);
            const auto indices = std::minmax(left, right);
            auto& first = locks_->stripes[indices.first];
            if (!first.lock()) return batch_status::error;
            first_guard = std::unique_lock<acl::fiber_mutex>(first, std::adopt_lock);
            if (indices.first != indices.second) {
                auto& second = locks_->stripes[indices.second];
                if (!second.lock()) return batch_status::error;
                second_guard = std::unique_lock<acl::fiber_mutex>(second,
                    std::adopt_lock);
            }
        } else if (!locks_->lock_keys(keys, key_count, guards)) {
            return batch_status::error;
        }
        detail::root_workspace workspace(key_count);
        auto& roots = workspace.roots();
        roots.reserve(key_count);
#ifdef HAS_TBB
        page_reader native_reader;
        auto* native_tbb = key_count == 1 &&
            typeid(*this) == typeid(mdb_tbb) ? static_cast<mdb_tbb*>(this) : nullptr;
#endif
        for (size_t i = 0; i < key_count; ++i) {
            const auto& key = keys[i];
            std::string bytes;
            lookup_status status;
            if (scope && scope->root_available_) {
                // Consume the snapshot before executing a command that could
                // fail or edit it. Only a committed write can refill it.
                scope->root_available_ = false;
#ifdef HAS_TBB
                if (native_tbb && scope->reader_available_)
                    native_reader = scope->reader_;
#endif
                bytes.swap(scope->committed_root_);
                status = scope->root_found_ ? lookup_status::found :
                    lookup_status::missing;
            } else {
                workspace.read_buffer(bytes);
#ifdef HAS_TBB
                if (native_tbb) {
                    if (scope && scope->reader_available_) native_reader = scope->reader_;
                    const auto* stored = native_tbb->borrow_root(key, native_reader);
                    // The scope holds the logical lock. A compact root can
                    // reuse this record even without inspecting any pages.
                    if (scope && stored && !scope->reader_available_) {
                        scope->reader_ = native_reader;
                        scope->reader_available_ = true;
                    }
                    status = stored ? lookup_status::found : lookup_status::missing;
                    if (stored && read_value) bytes = *stored;
                } else
#endif
                {
                    status = read_value ? dblookup(key, bytes) :
                        dbprobe_root(key, bytes);
                }
            }
            if (status == lookup_status::error) {
                return batch_status::error;
            }
            if (skip_missing && status == lookup_status::missing) {
                if (scope && scope->cache_root_) {
                    scope->root_found_ = false;
                    scope->root_available_ = true;
                }
                workspace.retain_buffer(bytes);
                return batch_status::ok;
            }
            if (!read_value) bytes.clear();
            roots.emplace_back(key, status == lookup_status::found,
                std::move(bytes));
        }
        if (root && key_count == 1) {
            const auto choice = (*root)(roots.front());
            if (choice == page_root_result::rejected)
                return batch_status::aborted;
            if (choice == page_root_result::ready) {
                if (!read_value && roots.front().action != update_action::write)
                    return batch_status::aborted;
                if (!page_transaction::valid_roots(
                    batch_view(roots.data(), roots.size())))
                    return batch_status::aborted;
                const page_reader* bound = nullptr;
#ifdef HAS_TBB
                if (native_tbb && native_reader.read_ &&
                    roots.front().action == update_action::write)
                    bound = &native_reader;
#endif
                return commit_pages(roots, workspace.pages(), observe,
                    ordered, count, scope, bound, deadline, edit);
            }
        }
        if (!read_value) return batch_status::aborted;
        auto& pages = workspace.pages();
        page_transaction transaction(*this,
            batch_view(roots.data(), roots.size()), &pages,
            scope && scope->cache_collections_ ? scope : nullptr,
            workspace.page_owner_buffer());
#ifdef HAS_TBB
        if (native_reader.read_) {
            transaction.reader_ = native_reader;
            transaction.reader_owner_ = roots.data();
        }
#endif
        const bool accepted = callback(transaction);
        if (transaction.storage_error_) {
            return batch_status::error;
        }
        if (!accepted || !transaction.valid_changes()) {
            return batch_status::aborted;
        }
        transaction.release_changes(pages);
        const auto* reader = pages.size() > 1 && roots.size() == 1 &&
            transaction.reader_supported_ &&
            transaction.reader_owner_ == roots.data() &&
            typeid(*this) == typeid(mdb) ? &transaction.reader_ : nullptr;
#ifdef HAS_TBB
        // oneTBB can also publish edits to existing sorted-set pages.
        if (native_tbb && roots.size() == 1 &&
            roots.front().action == update_action::write &&
            transaction.reader_supported_ && transaction.reader_owner_ == roots.data() &&
            (pages.size() <= 1 || roots.front().value.compare(0, 3, "PZR") == 0))
            reader = &transaction.reader_;
#endif
        return commit_pages(roots, pages, observe, ordered, count, scope,
            reader, deadline, edit);
    } catch (const std::bad_alloc&) {
        return batch_status::error;
    }
}

db::batch_status db::commit_pages(std::vector<batch_entry>& roots,
    std::vector<page_change>& pages, bool observe,
    const page_update* ordered, size_t count, page_lock_scope* scope,
    const page_reader* reader, const long long* deadline,
    const list_page_edit* edit) {
    assert(!deadline || roots.size() == 1);
    const bool editing_list = edit && edit->id;
    if (editing_list && (roots.size() != 1 || !pages.empty() ||
        roots[0].action != update_action::write || roots[0].clear_pages ||
        !scope || !scope->reader_available_ || !scope->cache_collections_ ||
        (watchers_ && watchers_->requires_page_payload())))
        return batch_status::error;
    bool changed = !pages.empty();
    for (const auto& root : roots) {
        changed = changed || root.action != update_action::keep;
    }
    if (!changed) {
        return batch_status::ok;
    }
    if (observe && execution_replica_read()) return batch_status::ok;
    if (observe && watchers_ && !watchers_->can_pages(roots, pages)) {
        return batch_status::unsupported;
    }
    const auto ready = prepare_ready(roots, ordered, count, &pages);
    if (!ready.empty()) ready_registry_->prepare(ready.data(), ready.size());
    if (scope) {
        scope->scored_member_ = nullptr;
        scope->zadd_score_mask_ = 0;
        scope->score_batch_size_ = 0;
        scope->membership_size_ = 0;
        scope->hash_valid_ = false;
        scope->hash_batch_size_ = 0;
        scope->lower_boundary_.valid = false;
        scope->upper_boundary_.valid = false;
        scope->range_end_ = 0;
        scope->list_end_.id = 0;
    }
    if (scope && scope->nodes_ &&
        (!pages.empty() || roots.front().clear_pages ||
            roots.front().action == update_action::erase)) {
        scope->nodes_.reset();
    }
    if (scope && scope->map_groups_) {
        if (roots.front().clear_pages || roots.front().action == update_action::erase) {
            scope->map_groups_.reset();
        } else {
            for (const auto& page : pages)
                for (auto& cached : scope->map_groups_->groups)
                    if (cached.id == page.id) cached.id = 0;
        }
    }
    if (scope && roots.front().action == update_action::erase)
        scope->native_root_ = nullptr;
    if (scope && scope->reader_available_ &&
        (roots.front().action == update_action::erase ||
            (scope->cache_root_ && (roots.front().clear_pages ||
             (!mdb::reader_has_more_pages(scope->reader_, pages.size()) &&
              std::any_of(pages.begin(), pages.end(), [](const page_change& page) {
                 return page.action == update_action::erase;
              })))))) {
        // oneTBB binds the record, which stays live until root deletion.
        // MDB binds the page index, which may disappear with its last page.
        scope->reader_available_ = false;
    }
    auto status = batch_status::unsupported;
    bool adopted = false;
#ifdef HAS_TBB
    // A native List edit publishes a page change through its plan.
    if (reader && pages.empty() && !editing_list) {
        status = static_cast<mdb_tbb*>(this)->replace_bound_root(roots.front(), *reader);
    } else
#endif
    if (editing_list) {
#ifdef HAS_TBB
        if (!scope->cache_root_) {
            status = mdb_tbb::edit_bound_list_page(roots[0], *edit, scope->reader_);
        } else
#endif
        {
            status = static_cast<mdb*>(this)->edit_bound_list_page(roots[0],
                *edit, scope->reader_, scope->native_root_);
        }
    } else if (roots.size() == 1 && pages.size() == 1 &&
        roots[0].action == update_action::write &&
        (!roots[0].clear_pages || (roots[0].found &&
            roots[0].value.compare(0, 4, "PSR\1", 4) == 0)) &&
        pages[0].action == update_action::write &&
        pages[0].owner == roots[0].key) {
        const auto& payload = pages[0].value;
        // Retain the compact allocation when copying into the replacement
        // workspace could grow a nearly full list page past its size class.
        const bool compact_list = ((payload.size() >= 960 &&
            payload.capacity() < 1024) || (payload.size() >= 480 &&
            payload.capacity() < 512)) &&
            list_page_codec::is_compact_page(payload);
        // Fragment overwrites can copy into the existing page allocation.
        // Keeping that allocation avoids churning large value buffers.
        const bool large_adoption = payload.size() >= 1024 &&
            (payload.compare(0, 4, "PMB\1", 4) != 0 ||
                roots[0].value.compare(0, 4, "PSR\1", 4) == 0);
        adopted = (large_adoption || compact_list) &&
            (!observe || !watchers_ || !watchers_->requires_page_payload());
#ifdef HAS_TBB
        // Multi-page readers are MDB bindings and take the path below.
        if (reader) {
            adopted = adopted && roots[0].value.compare(0, 4, "PSR\1", 4) == 0 &&
                payload.capacity() - payload.size() <= payload.size() / 4;
            status = static_cast<mdb_tbb*>(this)->replace_bound_page(
                roots[0], pages[0], *reader, adopted ? &pages[0].value : nullptr);
            adopted = adopted && status == batch_status::ok;
        } else
#endif
        if (scope && scope->cache_root_ && scope->reader_available_) {
            status = static_cast<mdb*>(this)->replace_bound_page(
                roots[0], pages[0], scope->reader_, scope->native_root_, adopted);
        } else if (adopted) {
            status = dbadopt_page(roots[0], pages[0]);
            adopted = status == batch_status::ok;
        }
    }
    if (status == batch_status::unsupported && !pages.empty() &&
        (pages.size() >= 32 || pages.front().value.size() > string_page_codec::inline_bytes) &&
        pages.front().owner == pages.back().owner) {
        const auto& owner = pages.front().owner;
        const auto root = std::find_if(roots.begin(), roots.end(),
            [&](const batch_entry& entry) { return entry.key == owner; });
        if (root != roots.end() && root->clear_pages &&
            root->action == update_action::write &&
            (pages.size() >= 32 || root->value.compare(0, 4, "PSR\1", 4) == 0) &&
            (!observe || !watchers_ || !watchers_->requires_page_payload())) {
            status = dbadopt_page_family(roots, pages);
            adopted = status == batch_status::ok;
        }
    }
#ifdef PKV_PACKED_RESP_ADOPTION
    if (status == batch_status::unsupported && roots.size() == 1 &&
        (!observe || !watchers_ || !watchers_->requires_page_payload())) {
        status = packed_resp_adopt(*this,roots[0],pages);
        adopted = status == batch_status::ok;
    }
#endif
    if (status == batch_status::unsupported && reader &&
#ifdef HAS_TBB
        pages.size() > 1 &&
#endif
        !roots[0].clear_pages &&
        roots[0].action == update_action::write) {
#ifdef HAS_TBB
        if (typeid(*this) == typeid(mdb_tbb))
            status = static_cast<mdb_tbb*>(this)->edit_bound_zset_pages(roots[0], pages, *reader);
        else
#endif
            status = static_cast<mdb*>(this)->apply_bound_pages(roots, pages, *reader,
                scope ? &scope->native_root_ : nullptr);
    }
    if (status == batch_status::unsupported) status = dbapply_pages(roots, pages);
    if (status == batch_status::ok) {
        if (editing_list && edit->length_bytes != list_page_codec::fragment_header_bytes) {
            // Only a committed compact edit can update the validated endpoint.
            scope->list_end_.id = edit->id;
            const auto stride = edit->right_stride;
            scope->list_end_.bytes = stride ? edit->expected_bytes - stride :
                edit->expected_bytes + edit->added_bytes();
            scope->list_end_.count = stride ? edit->expected_count - 1 :
                edit->expected_count + 1;
            scope->list_end_.uniform_pop = stride != 0;
            scope->list_end_.left = edit->left;
            scope->list_end_.length_bytes = edit->length_bytes;
        }
        publish_page_effects(roots, pages, ready, deadline,
            editing_list && edit->preserves_deadline);
        if (observe && !adopted) observe_pages(roots, pages);
        // A bound oneTBB record also pins the committed root under this lock.
        if (scope && (scope->cache_root_ ||
            (scope->cache_collections_ && scope->reader_available_))) {
            auto& root = roots.front();
            if (root.action == update_action::write && root.value.size() <= 256) {
                scope->committed_root_.swap(root.value);
                scope->root_found_ = scope->root_available_ = true;
            } else if (root.action == update_action::erase) {
                scope->committed_root_.clear();
                scope->root_found_ = false;
                scope->root_available_ = true;
            }
        }
    }
    return status;
}

db::batch_status db::copy_pages_to(const std::string& source, db& destination,
    const std::string& target, const page_copy_callback& callback) {
    execution_guard admission;
    if (!dbsupports_pages() || !destination.dbsupports_pages()) {
        return batch_status::unsupported;
    }
    try {
        if (this == &destination) {
            if (source == target) return batch_status::aborted;
            return transact_pages({source, target},
                [&](page_transaction& view) {
                const auto* from = view.roots().find(source);
                const std::string original = from->value;
                const bool accepted = callback(view, view);
                if (from->action != update_action::keep || from->clear_pages ||
                    from->value != original) return false;
                if (view.changes_owner(source)) return false;
                return accepted;
            });
        }
        const bool source_first = std::less<db*>()(this, &destination);
        auto& first = source_first ? locks_->for_key(source) :
            destination.locks_->for_key(target);
        auto& second = source_first ? destination.locks_->for_key(target) :
            locks_->for_key(source);
        if (!first.lock()) return batch_status::error;
        std::unique_lock<acl::fiber_mutex> first_guard(first, std::adopt_lock);
        if (!second.lock()) return batch_status::error;
        std::unique_lock<acl::fiber_mutex> second_guard(second,
            std::adopt_lock);
        std::string from, to;
        const auto source_status = dblookup(source, from);
        const auto target_status = destination.dblookup(target, to);
        if (source_status == lookup_status::error ||
            target_status == lookup_status::error) return batch_status::error;
        std::vector<batch_entry> source_roots, target_roots;
        source_roots.emplace_back(source, source_status == lookup_status::found,
            from);
        target_roots.emplace_back(target, target_status == lookup_status::found,
            std::move(to));
        page_transaction source_view(*this, batch_view(source_roots.data(), 1));
        page_transaction target_view(destination,
            batch_view(target_roots.data(), 1));
        const bool accepted = callback(source_view, target_view);
        if (source_view.storage_error_ || target_view.storage_error_) {
            return batch_status::error;
        }
        if (!accepted || source_view.failed_ || !target_view.valid_changes() ||
            source_view.has_changes() ||
            source_roots.front().action != update_action::keep ||
            source_roots.front().clear_pages ||
            source_roots.front().value != from) return batch_status::aborted;
        auto pages = target_view.release_changes();
        return destination.commit_pages(target_roots, pages, true);
    } catch (const std::bad_alloc&) {
        return batch_status::error;
    }
}

db::batch_status db::copy_to(const std::string& source, db& destination, const
    std::string& target, const copy_callback& callback) {
    execution_guard admission;
    if (this == &destination) return transact({source, target}, [&](batch_view
        entries) {
        const auto& from = *entries.find(source);
        auto& to = *entries.find(target);
        return callback(from.found, from.value, to.found, to.value, to.action);
    });
    const bool source_first = std::less<db*>()(this, &destination);
    auto& first = source_first ? locks_->for_key(source) :
        destination.locks_->for_key(target);
    auto& second = source_first ? destination.locks_->for_key(target) :
        locks_->for_key(source);
    if (!first.lock()) return batch_status::error;
    std::unique_lock<acl::fiber_mutex> first_guard(first, std::adopt_lock);
    if (!second.lock()) return batch_status::error;
    std::unique_lock<acl::fiber_mutex> second_guard(second, std::adopt_lock);
    std::string from, to;
    const auto source_status = dblookup(source, from);
    const auto target_status = destination.dblookup(target, to);
    if (source_status == lookup_status::error || target_status ==
        lookup_status::error) return batch_status::error;
    if (watchers_) watchers_->on_get(source, from, source_status ==
        lookup_status::found);
    if (destination.watchers_) destination.watchers_->on_get(target, to,
        target_status == lookup_status::found);
    update_action action = update_action::keep;
    if (!callback(source_status == lookup_status::found, from, target_status ==
        lookup_status::found, to, action)) return batch_status::aborted;
    if (action == update_action::keep || (action == update_action::erase &&
        target_status == lookup_status::missing)) return batch_status::ok;
    const bool ok = destination.apply_root(target,
        action == update_action::write ? &to : nullptr);
    if (!ok) return batch_status::error;
    destination.track_expiry(target, action == update_action::erase ?
        nullptr : &to);
    destination.invalidate_watch(target,
        action == update_action::erase);
    if (destination.watchers_) {
        if (action == update_action::write)
            destination.watchers_->on_set(target, to, true);
        else destination.watchers_->on_del(target, true);
    }
    return batch_status::ok;
}

db_cursor *db::create_cursor() {
    execution_guard admission;
    return this->dbcreate_cursor();
}

bool db::scan(db_cursor& cursor, std::vector<std::string>& keys, size_t max) {
    execution_guard admission;
    keys.clear();

    while (true) {
        size_t idx = cursor.get_db();
        if (idx >= this->dbsize()) {
            return true;
        }

        if (!this->dbscan(idx, cursor, keys, max)) {
            return false;
        }

        if (keys.size() >= max) {
            return true;
        }

        // If the keys are not enough, we should use the next db to get more,
        // and clear the current cursor.
        cursor.clear();
        cursor.next_db();
    }
}

} // namespace pkv
