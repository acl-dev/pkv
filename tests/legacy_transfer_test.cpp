#include "stdafx.h"
#include "dao/json/json_key.h"
#include "dao/paged_string.h"
#include "dao/paged_list.h"
#include "dao/paged_hash.h"
#include "dao/paged_set.h"
#include "dao/paged_zset.h"
#include "db/mdb/mdb.h"
#include "db/db_watcher.h"
#include "rpc/page_batch.h"
#include "coder/string_page.h"
#include <cstdlib>
#include <iostream>
#include <new>
#include <climits>

static thread_local long budget = -1;
void* operator new(std::size_t size) {
    if (!budget) throw std::bad_alloc();
    if (budget > 0) --budget;
    if (void* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
#if __cplusplus >= 201402L
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
#endif

using namespace pkv;
static const long long deadline = 4102444800123LL;
static void require(bool ok, const char* text) {
    if (!ok) { std::cerr << text << '\n'; std::exit(1); }
}
static std::string name(unsigned kind) {
    return "legacy-transfer-" + std::to_string(kind);
}
static value_record record(unsigned kind, long long expiry = -1) {
    value_record out;
    out.header.kind = static_cast<value_kind>(kind);
    out.header.expires_at_ms = expiry;
    const std::string large(70000, 'x');
    out.string_value = large;
    out.list_value = {"", std::string("a\0b", 3), large, "tail"};
    out.hash_value = {{"", ""}, {"field", large}};
    out.set_value = {"", large};
    out.zset_value = {{"", -1.25}, {large, 2.5}};
    return out;
}
static std::string seed(shared_db database, unsigned kind,
    long long expiry = -1) {
    std::string bytes;
    require(database->value_codec().encode(record(kind, expiry), bytes) ==
        codec_status::ok && database->set(name(kind), bytes), "legacy seed");
    return bytes;
}
static std::string root(shared_db database, unsigned kind,
    const std::string& target = "") {
    std::string bytes;
    require(database->lookup(target.empty() ? name(kind) : target, bytes) ==
        db::lookup_status::found,
        "missing root");
    return bytes;
}
static watch_token watch(shared_db database, unsigned kind,
    const std::string& target = "") {
    std::vector<watch_token> tokens;
    require(database->watch({target.empty() ? name(kind) : target}, tokens) ==
        db::batch_status::ok,
        "WATCH seed");
    return tokens.front();
}
static void verify(shared_db database, unsigned kind, long long expiry,
    const std::string& target = "") {
    const auto key = target.empty() ? name(kind) : target;
    const auto bytes = root(database, kind, key);
    std::unique_ptr<record_view> view;
    require(bytes.size() <= page_transaction::max_value_bytes &&
        database->value_codec().decode_stored(bytes, view) ==
            codec_status::ok && view->paged_payload() &&
        view->header().expires_at_ms == expiry, "unbounded/expired root");
    const auto expected = record(kind);
    switch (expected.header.kind) {
    case value_kind::string: {
        dao::paged_string strings; std::string out;
        require(strings.get(database, key, out) &&
            out == expected.string_value, "String migration"); break;
    }
    case value_kind::list: {
        dao::paged_list lists; std::vector<std::string> out;
        require(lists.range(database, key, 0, -1, out) &&
            out == expected.list_value, "List migration"); break;
    }
    case value_kind::hash: {
        dao::paged_hash hashes;
        require(hashes.hgetall(database, key) &&
            hashes.get_fields() == expected.hash_value, "Hash migration");
        break;
    }
    case value_kind::set: {
        dao::paged_set sets; std::set<std::string> out;
        require(sets.members(database, key, out) &&
            out == expected.set_value, "Set migration"); break;
    }
    case value_kind::zset: {
        dao::paged_zset sets; std::vector<zset_member> out;
        require(sets.range(database, key, 0, -1, false, out) &&
            out.size() == 2 && out[0].member.empty() &&
            out[0].score == -1.25 && out[1].member ==
                expected.zset_value.rbegin()->first && out[1].score == 2.5,
            "ZSet migration"); break;
    }
    }
}
static void orphan(shared_db database, const std::string& key, bool create) {
    require(database->transact_pages({key}, [&](page_transaction& tx) {
        if (create) return tx.put(key, 9999, "old orphan");
        std::string bytes;
        return tx.lookup(key, 9999, bytes) == db::lookup_status::missing;
    }) == db::batch_status::ok, "old page family survived replacement");
}
static void transfers(shared_db database) {
    dao::json_key keys; dao::paged_string strings;
    for (unsigned kind = 0; kind < 5; ++kind) {
        const auto before = seed(database, kind, deadline);
        const auto copy = name(kind) + "-copy", moved = name(kind) + "-moved";
        require(strings.set(database, copy, std::string(110000, 'o')) &&
            strings.set(database, moved, std::string(110000, 'p')),
            "old destination page families");
        orphan(database, copy, true); orphan(database, moved, true);
        auto source_token = watch(database, kind);
        auto target_token = watch(database, kind, copy);
        const auto target_before = root(database, kind, copy);
        bool changed = true;
        require(keys.rename(database, name(kind), name(kind), false,
            changed) && changed && root(database, kind) == before &&
            source_token->valid(unix_time_ms()), "same-key RENAME changed");
        require(keys.rename(database, name(kind), copy, true, changed) &&
            !changed && keys.copy(database, name(kind), database, copy,
                false, changed) && !changed &&
            root(database, kind, copy) == target_before &&
            root(database, kind) == before &&
            source_token->valid(unix_time_ms()) &&
            target_token->valid(unix_time_ms()), "conditional transfer wrote");
        require(keys.copy(database, name(kind), database, copy, true,
            changed) && changed && root(database, kind) == before &&
            source_token->valid(unix_time_ms()) &&
            !target_token->valid(unix_time_ms()), "COPY source/WATCH");
        verify(database, kind, deadline, copy);
        orphan(database, copy, false);
        auto moved_token = watch(database, kind, moved);
        require(keys.rename(database, name(kind), moved, false, changed) &&
            changed && !source_token->valid(unix_time_ms()) &&
            !moved_token->valid(unix_time_ms()), "RENAME WATCH");
        std::string bytes;
        require(database->lookup(name(kind), bytes) ==
            db::lookup_status::missing, "RENAME retained source");
        verify(database, kind, deadline, moved);
        orphan(database, moved, false);
        require(keys.copy(database, name(kind), database, copy, true,
            changed) && !changed, "missing COPY changed target");
        verify(database, kind, deadline, copy);
    }
}
class failing_db : public mdb {
public:
    failing_db() : mdb(4) {}
    bool reject = false, pages = true, unsupported = false;
    unsigned commits = 0;
protected:
    bool dbsupports_pages() const override { return pages; }
    batch_status dbapply_pages(const std::vector<batch_entry>& roots,
        const std::vector<page_change>& changes) override {
        if (reject) return unsupported ? batch_status::unsupported :
            batch_status::error;
        const auto result = mdb::dbapply_pages(roots, changes);
        if (result == batch_status::ok) ++commits;
        return result;
    }
};
static bool transfer(dao::json_key& keys, shared_db database, unsigned kind,
    bool rename, bool& changed) {
    return rename ? keys.rename(database, name(kind), "target", false,
        changed) : keys.copy(database, name(kind), database, "target", true,
            changed);
}
static void failures() {
    for (bool unsupported : {false, true})
    for (unsigned kind = 0; kind < 5; ++kind)
    for (bool rename : {false, true}) {
        auto source = std::make_shared<failing_db>();
        require(source->open(""), "failure DB open");
        shared_db database = source;
        const auto before = seed(database, kind, deadline);
        dao::paged_string strings;
        require(strings.set(database, "target", std::string(110000, 'o')),
            "failure target");
        const auto target_before = root(database, kind, "target");
        auto token = watch(database, kind), target = watch(database, kind,
            "target");
        dao::json_key keys; bool changed = true;
        source->reject = true; source->unsupported = unsupported;
        require(!transfer(keys, database, kind, rename, changed) && !changed &&
            keys.status() == (unsupported ?
                dao::dao_base::read_status::unsupported_operation :
                dao::dao_base::read_status::storage_error) &&
            root(database, kind) == before &&
            root(database, kind, "target") == target_before &&
            token->valid(unix_time_ms()) && target->valid(unix_time_ms()),
            "failed transfer exposed changes");
        source->reject = false;
        require(transfer(keys, database, kind, rename, changed) && changed,
            "transfer retry");
        verify(database, kind, deadline, "target");
    }
}
static long allocation_count(unsigned kind, bool rename) {
    shared_db database = std::make_shared<failing_db>();
    require(database->open(""), "allocation DB open");
    seed(database, kind, deadline);
    dao::json_key keys; bool changed = false;
    budget = LONG_MAX;
    const bool ok = transfer(keys, database, kind, rename, changed);
    const auto used = LONG_MAX - budget;
    budget = -1;
    require(ok && changed && used > 0 && used < 3000, "allocation count");
    return used;
}
static void allocation_failures() {
    for (unsigned kind = 0; kind < 5; ++kind)
    for (bool rename : {false, true}) {
        const auto count = allocation_count(kind, rename);
        unsigned completed = 0, rejected = 0;
        for (long fault = 0; fault <= count; ++fault) {
            auto source = std::make_shared<failing_db>();
            require(source->open(""), "fault DB open");
            shared_db database = source;
            const auto before = seed(database, kind, deadline);
            auto token = watch(database, kind);
            dao::json_key keys; bool changed = false, ok = false;
            budget = fault;
            try { ok = transfer(keys, database, kind, rename, changed); }
            catch (const std::bad_alloc&) {}
            budget = -1;
            if (ok && changed) {
                ++completed;
                require(source->commits == 1 &&
                    token->valid(unix_time_ms()) == !rename,
                    "successful transfer fault/WATCH");
                verify(database, kind, deadline, "target");
            } else {
                ++rejected;
                std::string bytes;
                require(!source->commits && root(database, kind) == before &&
                    database->lookup("target", bytes) ==
                        db::lookup_status::missing &&
                    token->valid(unix_time_ms()), "OOM exposed transfer");
            }
        }
        require(completed && rejected, "allocation sweep coverage");
        std::cout << "Transfer allocation positions: " << kind << ' ' <<
            rename << ' ' << count + 1 << '\n';
    }
}
static void compatibility() {
    auto source = std::make_shared<failing_db>();
    require(source->open(""), "nonpage open");
    shared_db database = source; source->pages = false;
    dao::json_key keys;
    for (unsigned kind = 0; kind < 5; ++kind) {
        const auto before = seed(database, kind, deadline);
        bool changed;
        require(keys.copy(database, name(kind), database, "target", true,
            changed) && changed && root(database, kind, "target") == before &&
            keys.rename(database, name(kind), "moved", false, changed) &&
            changed && root(database, kind, "moved") == before,
            "nonpage opaque transfer compatibility");
    }
}
static void cross_database() {
    shared_db source = std::make_shared<failing_db>();
    shared_db destination = std::make_shared<failing_db>();
    require(source->open("") && destination->open(""), "cross-DB open");
    dao::json_key keys;
    for (unsigned kind = 0; kind < 5; ++kind) {
        const auto before = seed(source, kind, deadline);
        auto token = watch(source, kind);
        bool changed;
        require(keys.copy(source, name(kind), destination, name(kind), true,
            changed) && changed && root(source, kind) == before &&
            token->valid(unix_time_ms()), "cross-DB COPY changed source");
        verify(destination, kind, deadline);
    }
}
class format_change_db : public mdb {
public:
    explicit format_change_db(unsigned remove_at = 0)
    : mdb(4), remove_at_(remove_at) {
        require(default_record_codec()->encode(record(1, deadline), wide) ==
            codec_status::ok, "format race wide encode");
        value_record zero; zero.header.kind = value_kind::hash;
        require(default_record_codec()->encode(zero, empty) == codec_status::ok,
            "format race empty encode");
    }
    std::string wide, empty;
protected:
    lookup_status dblookup(const std::string& key, std::string& out) override {
        if (key == "a" && !removed_) {
            ++reads_;
            if (reads_ == remove_at_) {
                require(mdb::dbdel(key), "race source deletion");
                removed_ = true;
            } else if (reads_ == 2) {
                require(mdb::dbset(key, empty), "race to opaque");
            } else if (reads_ == 3) {
                require(mdb::dbset(key, wide), "race to native");
            }
        }
        return mdb::dblookup(key, out);
    }
private:
    unsigned reads_ = 0, remove_at_;
    bool removed_ = false;
};
static void format_change_retry() {
    for (bool rename : {false, true}) {
        auto source = std::make_shared<format_change_db>();
        require(source->open("") && source->set("a", source->wide),
            "format race seed");
        shared_db database = source;
        dao::json_key keys; bool changed;
        require(rename ? keys.rename(database, "a", "b", false, changed) :
            keys.copy(database, "a", database, "b", true, changed),
            "format race transfer");
        require(changed, "format race did not transfer");
        verify(database, 1, deadline, "b");
    }
}
static void disappearance_retry() {
    for (unsigned phase : {2U, 3U}) for (bool nx : {false, true})
    for (bool native : {false, true}) {
        auto source = std::make_shared<format_change_db>(phase);
        if (native) {
            string_root root; root.length = 6; root.inline_value = "native";
            require(string_page_codec::encode_root(root, source->wide) ==
                string_page_status::ok, "native disappearance seed");
        }
        require(source->open("") && source->set("a", source->wide) &&
            source->set("b", source->wide), "disappearance seed");
        shared_db database = source;
        dao::json_key keys; bool changed = true;
        require(!keys.rename(database, "a", "b", nx, changed) && !changed &&
            keys.status() == dao::dao_base::read_status::no_key &&
            root(database, 1, "b") == source->wide,
            "disappeared source did not report no such key");
        std::string bytes;
        require(database->lookup("a", bytes) == db::lookup_status::missing,
            "disappeared source was restored");
    }
}
class transfer_observer : public db_watcher {
public:
    unsigned groups = 0;
    std::shared_ptr<const page_batch> last;
    bool supports_pages() const override { return true; }
    bool on_pages_database(unsigned database,
        const std::vector<db::batch_entry>& roots,
        const std::vector<page_change>& pages) override {
        last = page_batch::capture(database, roots, pages);
        require(last && database == 7, "transfer batch capture");
        for (const auto& root : last->roots())
            require(root.value.size() <= page_transaction::max_value_bytes,
                "transfer emitted a large root");
        ++groups; return true;
    }
    bool on_set(const std::string&, const std::string&, bool) override {
        return true;
    }
    bool on_get(const std::string&, const std::string&, bool) override {
        return true;
    }
    bool on_del(const std::string&, bool) override { return true; }
};
static void publication() {
    transfer_observer observer;
    db_watchers watchers(7); watchers.add_watcher(&observer);
    shared_db source = std::make_shared<failing_db>();
    shared_db mirror = std::make_shared<failing_db>();
    require(source->open("", &watchers) && mirror->open(""),
        "publication DBs open");
    dao::json_key keys;
    for (unsigned kind = 0; kind < 5; ++kind) {
        const auto before = seed(source, kind, deadline);
        seed(mirror, kind, deadline);
        auto token = watch(mirror, kind);
        bool changed;
        require(keys.copy(source, name(kind), source, "copy", true, changed)
            && changed && observer.groups == kind * 2 + 1 &&
            observer.last->roots().size() == 1 &&
            observer.last->apply(*mirror) == db::batch_status::ok &&
            root(mirror, kind) == before && token->valid(unix_time_ms()),
            "COPY publication changed source");
        verify(mirror, kind, deadline, "copy");
        require(keys.rename(source, name(kind), "moved", false, changed) &&
            changed && observer.groups == kind * 2 + 2 &&
            observer.last->roots().size() == 2 &&
            observer.last->apply(*mirror) == db::batch_status::ok &&
            !token->valid(unix_time_ms()), "RENAME publication/WATCH");
        verify(mirror, kind, deadline, "moved");
        std::string bytes;
        require(mirror->lookup(name(kind), bytes) == db::lookup_status::missing,
            "RENAME publication retained source");
    }
}
class opaque_observer : public transfer_observer {
public:
    bool supports_pages() const override { return false; }
};
static void legacy_observation() {
    opaque_observer observer;
    db_watchers watchers(7); watchers.add_watcher(&observer);
    shared_db database = std::make_shared<failing_db>();
    require(database->open("", &watchers), "opaque observer open");
    dao::json_key keys;
    for (unsigned kind = 0; kind < 5; ++kind) {
        const auto before = seed(database, kind, deadline);
        bool changed;
        require(keys.copy(database, name(kind), database, "copy", true,
            changed) && changed && root(database, kind, "copy") == before &&
            keys.rename(database, name(kind), "moved", false, changed) &&
            changed && root(database, kind, "moved") == before &&
            !observer.groups, "legacy observer received native pages");
    }
}
class transfer_view : public record_view {
public:
    transfer_view(std::unique_ptr<record_view> inner, bool migrate)
    : record_view(inner->header()), inner_(std::move(inner)),
      migrate_(migrate) {}
    bool native_importable() const override { return migrate_; }
    bool valid_payload() const override { return inner_->valid_payload(); }
    bool string_value(std::string&) const override { return false; }
    field_status hash_field(const std::string& key,
        std::string& out) const override {
        return inner_->hash_field(key, out);
    }
    bool hash_values(std::map<std::string, std::string>&) const override {
        return false;
    }
    bool hash_size(size_t& count) const override {
        return inner_->hash_size(count);
    }
    bool hash_visit(const hash_visitor& visit) const override {
        return migrate_ && inner_->hash_visit(visit);
    }
private:
    std::unique_ptr<record_view> inner_;
    bool migrate_;
};
class transfer_codec : public record_codec {
public:
    explicit transfer_codec(bool migrate) : migrate_(migrate) {}
    codec_status encode(const value_record&, std::string&) const override {
        return codec_status::unsupported_version;
    }
    codec_status decode(const std::string& bytes,
        std::unique_ptr<record_view>& out) const override {
        std::unique_ptr<record_view> view;
        const auto result = default_record_codec()->decode(bytes, view);
        if (result != codec_status::ok) return result;
        out.reset(new transfer_view(std::move(view), migrate_));
        return codec_status::ok;
    }
private:
    bool migrate_;
};
static void extension_views() {
    for (bool migrate : {false, true}) {
        shared_db database = std::make_shared<failing_db>();
        require(database->open("", nullptr,
            std::make_shared<transfer_codec>(migrate)), "extension DB open");
        std::string before;
        require(default_record_codec()->encode(record(1, deadline), before) ==
            codec_status::ok && database->set(name(1), before),
            "extension Hash seed");
        dao::json_key keys; bool changed;
        require(keys.copy(database, name(1), database, "copy", true, changed)
            && changed && keys.rename(database, name(1), "moved", false,
                changed) && changed, "extension transfer");
        if (migrate) {
            verify(database, 1, deadline, "copy");
            verify(database, 1, deadline, "moved");
        } else {
            require(root(database, 1, "copy") == before &&
                root(database, 1, "moved") == before,
                "opaque read-only codec lost exact bytes");
        }
    }
}
int main(int argc, char** argv) {
    acl::acl_cpp_init();
    const std::string backend = argc > 1 ? argv[1] : "mdb";
    shared_db database;
    if (backend == "mdb") database = db::create_mdb(4);
    else if (backend == "tbb") database = db::create_mdb_tbb(4);
    else if (backend == "rdb") database = db::create_rdb();
    else if (backend == "wdb") database = db::create_wdb();
    require(database && database->open(argc > 2 ? argv[2] : ""), "DB open");
    if (argc > 3 && std::string(argv[3]) == "--reopen") {
        for (unsigned kind = 0; kind < 5; ++kind) {
            verify(database, kind, deadline, name(kind) + "-copy");
            verify(database, kind, deadline, name(kind) + "-moved");
            orphan(database, name(kind) + "-copy", false);
            orphan(database, name(kind) + "-moved", false);
        }
    } else {
        transfers(database);
        if (backend == "mdb") {
            failures(); allocation_failures(); compatibility();
            extension_views(); publication(); legacy_observation();
            cross_database(); format_change_retry(); disappearance_retry();
        }
    }
    std::cout << "Legacy transfers passed: " << backend << '\n';
}
