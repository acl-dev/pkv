#pragma once
#include "db/database_pool.h"
#include "db/move_recovery.h"
#include "db/move_journal.h"
#include "db/page_transaction.h"
#include "db/rocksdb/rdb.h"
#include "db/wt/wdb.h"
#include "coder/string_page.h"
#include <iostream>
#include <set>

using namespace pkv;
inline void require(bool ok, const char* reason) {
    if (!ok) { std::cerr << reason << '\n'; std::exit(1); }
}
inline const std::string& moved_key() {
    static const std::string value("move\0key\xff", 9); return value;
}
struct probe_metrics { size_t maximum_batch = 0, maximum_value = 0; };
template<class Base> struct recovery_probe : Base {
    static void injected(const std::string& name) {
        std::cerr << "FAULT " << name << '\n';
    }
    bool source;
    std::string fault;
    unsigned syncs = 0, writes = 0;
    probe_metrics metrics;
    recovery_probe(bool from, const std::string& failure)
    : source(from), fault(failure) {}
    db::batch_status visit(const std::string& owner,
        const db::page_visitor& callback) {
        return Base::dbvisit_pages(owner, callback);
    }
    db::lookup_status bounded(const std::string& owner, std::string& out) {
        return Base::dblookup_bounded_root(owner, out);
    }
    bool raw_page(const std::string& owner, uint64_t id,
        const std::string& bytes) {
        return Base::dbapply_pages({}, {page_change(owner, id, bytes,
            db::update_action::write)}) == db::batch_status::ok;
    }
protected:
    db::lookup_status dblookup_bounded_root(const std::string& key,
        std::string& out) override {
        if (source && fault == "source-read") {
            injected(fault); return db::lookup_status::error;
        }
        return Base::dblookup_bounded_root(key, out);
    }
    db::batch_status dbvisit_pages(const std::string& key,
        const db::page_visitor& callback) override {
        unsigned seen = 0;
        const auto result = Base::dbvisit_pages(key, [&](uint64_t id,
            const std::string& bytes) {
            if (source && fault == "source-stream" && ++seen == 2) {
                injected(fault); return false;
            }
            return callback(id, bytes);
        });
        return source && fault == "source-stream" && seen == 2 ?
            db::batch_status::error : result;
    }
    db::batch_status dbapply_pages(const std::vector<db::batch_entry>& roots,
        const std::vector<page_change>& pages) override {
        metrics.maximum_batch = std::max(metrics.maximum_batch, pages.size());
        for (const auto& page : pages) metrics.maximum_value = std::max(
            metrics.maximum_value, page.value.size());
        if (!source && ((fault == "target-page" && !pages.empty() &&
            ++writes == 2) || (fault == "target-root" && !roots.empty()))) {
            injected(fault); return db::batch_status::error;
        }
        const auto result = Base::dbapply_pages(roots, pages);
        if (!source && result == db::batch_status::ok &&
            ((fault == "target-page-after" && !pages.empty() && ++writes == 2)
            || (fault == "target-root-after" && !roots.empty()))) {
            injected(fault); return db::batch_status::error;
        }
        return result;
    }
    db::batch_status dbapply(const std::vector<db::batch_entry>& roots)
        override {
        if (source && fault == "source-erase") {
            injected(fault); return db::batch_status::error;
        }
        const auto result = Base::dbapply(roots);
        if (source && fault == "source-erase-after" &&
            result == db::batch_status::ok) {
            injected(fault); return db::batch_status::error;
        }
        return result;
    }
    db::sync_status dbsync_key(const std::string& key) override {
        ++syncs;
        if ((!source && fault == "target-sync") ||
            (source && fault == "source-sync" && syncs == 2)) {
            injected(fault); return db::sync_status::error;
        }
        return Base::dbsync_key(key);
    }
};
struct recovery_io : move_journal_io {
    std::string fault;
    bool armed = false;
    int remove(const char* path) override {
        if (armed && fault == "journal-remove") {
            std::cerr << "FAULT " << fault << '\n'; errno = EIO; return -1;
        }
        return move_journal_io::remove(path);
    }
    int sync(int fd) override {
        if (armed && fault == "journal-sync") {
            std::cerr << "FAULT " << fault << '\n'; errno = EIO; return -1;
        }
        return move_journal_io::sync(fd);
    }
};
inline std::string paths(const std::string& root, const char* role) {
    return root + "/" + role + "0;" + root + "/" + role + "1";
}
inline shared_db open_probe(const std::string& backend,
    const std::string& root, bool source, const std::string& fault) {
    shared_db value;
#ifdef HAS_ROCKSDB
    if (backend == "rdb") value.reset(new recovery_probe<rdb>(source, fault));
#endif
#ifdef HAS_WT
    if (backend == "wdb") value.reset(new recovery_probe<wdb>(source, fault));
#endif
    if (!source && fault == "unopened") return value;
    require(value && value->open(paths(root, source ? "from" : "to").c_str()),
        "recovery native open");
    return value;
}
inline db::batch_status visit(shared_db database, const std::string& owner,
    const db::page_visitor& callback) {
#ifdef HAS_ROCKSDB
    if (auto* value = dynamic_cast<recovery_probe<rdb>*>(database.get()))
        return value->visit(owner, callback);
#endif
#ifdef HAS_WT
    if (auto* value = dynamic_cast<recovery_probe<wdb>*>(database.get()))
        return value->visit(owner, callback);
#endif
    return db::batch_status::unsupported;
}
inline db::lookup_status bounded(shared_db database, const std::string& owner,
    std::string& out) {
#ifdef HAS_ROCKSDB
    if (auto* value = dynamic_cast<recovery_probe<rdb>*>(database.get()))
        return value->bounded(owner, out);
#endif
#ifdef HAS_WT
    if (auto* value = dynamic_cast<recovery_probe<wdb>*>(database.get()))
        return value->bounded(owner, out);
#endif
    return db::lookup_status::error;
}
inline bool raw_page(shared_db database, const std::string& owner,
    uint64_t id, const std::string& bytes) {
#ifdef HAS_ROCKSDB
    if (auto* value = dynamic_cast<recovery_probe<rdb>*>(database.get()))
        return value->raw_page(owner, id, bytes);
#endif
#ifdef HAS_WT
    if (auto* value = dynamic_cast<recovery_probe<wdb>*>(database.get()))
        return value->raw_page(owner, id, bytes);
#endif
    return false;
}
inline probe_metrics metrics(shared_db database) {
#ifdef HAS_ROCKSDB
    if (auto* value = dynamic_cast<recovery_probe<rdb>*>(database.get()))
        return value->metrics;
#endif
#ifdef HAS_WT
    if (auto* value = dynamic_cast<recovery_probe<wdb>*>(database.get()))
        return value->metrics;
#endif
    return {};
}
inline void reset_metrics(shared_db database) {
#ifdef HAS_ROCKSDB
    if (auto* value = dynamic_cast<recovery_probe<rdb>*>(database.get()))
        value->metrics = {};
#endif
#ifdef HAS_WT
    if (auto* value = dynamic_cast<recovery_probe<wdb>*>(database.get()))
        value->metrics = {};
#endif
}
inline std::string encoded_root(size_t count, int64_t expiry) {
    string_root value; value.pages = count; value.length = count * 16384;
    value.expires_at_ms = expiry;
    std::string bytes;
    require(string_page_codec::encode_root(value, bytes) ==
        string_page_status::ok, "recovery root encode");
    return bytes;
}
inline std::string page_bytes(uint64_t id) {
    std::string out(16384, static_cast<char>(id % 251));
    for (unsigned i = 0; i < 8; ++i) out[i] = char(id >> (8 * i));
    return out;
}
inline void seed(shared_db database, const std::string& owner, size_t count,
    int64_t expiry) {
    require(database->transact_pages({owner}, [&](page_transaction& tx) {
        auto& root = *tx.roots().find(owner);
        root.action = db::update_action::write;
        root.value = encoded_root(count, expiry);
        if (!tx.reset_pages(owner)) return false;
        for (uint64_t i = 1; i <= count; ++i)
            if (!tx.put(owner, i, page_bytes(i))) return false;
        return true;
    }) == db::batch_status::ok, "recovery seed commit");
}
inline void check(shared_db database, const std::string& owner, bool present,
    size_t count, int64_t expiry) {
    require(database->transact_pages({owner}, [&](page_transaction& tx) {
        const auto& root = *tx.roots().find(owner);
        require(root.found == present && (!present || root.value ==
            encoded_root(count, expiry)), "recovery root/expiry mismatch");
        return true;
    }) == db::batch_status::ok, "recovery root check");
    uint64_t next = 1;
    require(visit(database, owner, [&](uint64_t id, const std::string& bytes) {
        require(present && id == next++ && bytes == page_bytes(id),
            "recovery native page mismatch");
        return true;
    }) == db::batch_status::ok && next == (present ? count + 1 : 1),
        "recovery page family mismatch");
}
inline void index(shared_db database, const std::set<std::string>& expected) {
    uint64_t count = 0;
    require(database->key_count(count) == db::count_status::ok &&
        count == expected.size(), "recovery root count");
    std::set<std::string> keys;
    unsigned long long cursor = 0, next = 0;
    do {
        std::vector<std::string> page;
        require(database->scan_keys(cursor, 1, page, next) ==
            db::scan_status::ok, "recovery index scan");
        keys.insert(page.begin(), page.end()); cursor = next;
    } while (cursor);
    require(keys == expected, "recovery index mismatch");
}
