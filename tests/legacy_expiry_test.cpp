#include "stdafx.h"
#include "dao/json/json_key.h"
#include "dao/paged_string.h"
#include "dao/paged_list.h"
#include "dao/paged_hash.h"
#include "dao/paged_set.h"
#include "dao/paged_zset.h"
#include "db/mdb/mdb.h"
#include "db/db_watcher.h"
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
    return "legacy-expiry-" + std::to_string(kind);
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
static std::string root(shared_db database, unsigned kind) {
    std::string bytes;
    require(database->lookup(name(kind), bytes) == db::lookup_status::found,
        "missing root");
    return bytes;
}
static watch_token watch(shared_db database, unsigned kind) {
    std::vector<watch_token> tokens;
    require(database->watch({name(kind)}, tokens) == db::batch_status::ok,
        "WATCH seed");
    return tokens.front();
}
static void verify(shared_db database, unsigned kind, long long expiry) {
    const auto bytes = root(database, kind);
    std::unique_ptr<record_view> view;
    require(bytes.size() <= page_transaction::max_value_bytes &&
        database->value_codec().decode_stored(bytes, view) ==
            codec_status::ok && view->paged_payload() &&
        view->header().expires_at_ms == expiry, "unbounded/expired root");
    const auto expected = record(kind);
    switch (expected.header.kind) {
    case value_kind::string: {
        dao::paged_string strings; std::string out;
        require(strings.get(database, name(kind), out) &&
            out == expected.string_value, "String migration"); break;
    }
    case value_kind::list: {
        dao::paged_list lists; std::vector<std::string> out;
        require(lists.range(database, name(kind), 0, -1, out) &&
            out == expected.list_value, "List migration"); break;
    }
    case value_kind::hash: {
        dao::paged_hash hashes;
        require(hashes.hgetall(database, name(kind)) &&
            hashes.get_fields() == expected.hash_value, "Hash migration");
        break;
    }
    case value_kind::set: {
        dao::paged_set sets; std::set<std::string> out;
        require(sets.members(database, name(kind), out) &&
            out == expected.set_value, "Set migration"); break;
    }
    case value_kind::zset: {
        dao::paged_zset sets; std::vector<zset_member> out;
        require(sets.range(database, name(kind), 0, -1, false, out) &&
            out.size() == 2 && out[0].member.empty() &&
            out[0].score == -1.25 && out[1].member ==
                expected.zset_value.rbegin()->first && out[1].score == 2.5,
            "ZSet migration"); break;
    }
    }
}
static void migration(shared_db database) {
    dao::json_key keys;
    for (unsigned kind = 0; kind < 5; ++kind) {
        const auto before = seed(database, kind);
        auto token = watch(database, kind);
        require(!keys.set_expiry(database, name(kind), deadline,
            dao::key::expire_xx) && !keys.persist(database, name(kind)) &&
            root(database, kind) == before && token->valid(unix_time_ms()),
            "rejected condition migrated/invalidated legacy key");
        require(keys.set_expiry(database, name(kind), deadline,
            dao::key::expire_nx) && !token->valid(unix_time_ms()),
            "migration did not change WATCH");
        verify(database, kind, deadline);
        seed(database, kind, deadline);
        token = watch(database, kind);
        require(keys.persist(database, name(kind)) &&
            !token->valid(unix_time_ms()), "legacy PERSIST");
        verify(database, kind, -1);
    }
}
class failing_db : public mdb {
public:
    failing_db() : mdb(4) {}
    bool reject = false, pages = true;
    unsigned commits = 0;
protected:
    bool dbsupports_pages() const override { return pages; }
    batch_status dbapply_pages(const std::vector<batch_entry>& roots,
        const std::vector<page_change>& changes) override {
        if (reject) return batch_status::error;
        const auto result = mdb::dbapply_pages(roots, changes);
        if (result == batch_status::ok) ++commits;
        return result;
    }
};
static void failures() {
    auto source = std::make_shared<failing_db>();
    require(source->open(""), "failure DB open");
    shared_db database = source;
    dao::json_key keys;
    for (unsigned kind = 0; kind < 5; ++kind) {
        const auto before = seed(database, kind);
        auto token = watch(database, kind);
        source->reject = true;
        require(!keys.set_expiry(database, name(kind), deadline, 0) &&
            keys.status() == dao::dao_base::read_status::storage_error &&
            root(database, kind) == before && token->valid(unix_time_ms()),
            "failed migration leaked root/WATCH changes");
        source->reject = false;
        require(keys.set_expiry(database, name(kind), deadline, 0),
            "migration retry");
        verify(database, kind, deadline);
    }
    source->pages = false;
    seed(database, 0);
    require(keys.set_expiry(database, name(0), deadline, 0),
        "nonpage legacy expiry compatibility");
    std::unique_ptr<record_view> view;
    require(database->value_codec().decode_stored(root(database, 0), view) ==
        codec_status::ok && !view->paged_payload() &&
        view->header().expires_at_ms == deadline, "nonpage fallback root");
}
class migration_observer : public db_watcher {
public:
    unsigned groups = 0;
    bool supports_pages() const override { return true; }
    bool on_pages_database(unsigned database,
        const std::vector<db::batch_entry>& roots,
        const std::vector<page_change>& pages) override {
        require(database == 9 && roots.size() == 1 && !pages.empty() &&
            roots[0].clear_pages && !roots[0].maintenance_only &&
            roots[0].value.size() <= page_transaction::max_value_bytes,
            "migration observer root/history");
        for (const auto& page : pages)
            require(page.value.size() <= page_transaction::max_value_bytes,
                "migration observer unbounded page");
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
static void observation() {
    migration_observer observer;
    db_watchers watchers(9); watchers.add_watcher(&observer);
    shared_db database = std::make_shared<failing_db>();
    require(database->open("", &watchers), "observed DB open");
    dao::json_key keys;
    for (unsigned kind = 0; kind < 5; ++kind) {
        seed(database, kind);
        require(keys.set_expiry(database, name(kind), deadline, 0) &&
            observer.groups == kind + 1, "migration publication count");
    }
}
static long allocation_count(unsigned kind) {
    shared_db database = std::make_shared<failing_db>();
    require(database->open(""), "allocation count DB");
    seed(database, kind);
    auto token = watch(database, kind);
    dao::json_key keys;
    budget = LONG_MAX;
    const bool ok = keys.set_expiry(database, name(kind), deadline, 0);
    const auto used = LONG_MAX - budget;
    budget = -1;
    require(ok && used > 0 && used < 3000 &&
        !token->valid(unix_time_ms()), "allocation count range");
    return used;
}
static void allocation_failures() {
    for (unsigned kind = 0; kind < 5; ++kind) {
        const auto count = allocation_count(kind);
        unsigned completed = 0, rejected = 0;
        // Recycled buffers can change the allocation count between runs.
        // Cover the measured range and continue until publication succeeds.
        for (long fault = 0; fault < 3000 && (fault <= count || !completed);
            ++fault) {
            auto source = std::make_shared<failing_db>();
            require(source->open(""), "fault DB open");
            shared_db database = source;
            const auto before = seed(database, kind);
            auto token = watch(database, kind);
            dao::json_key keys;
            bool ok = false;
            budget = fault;
            try { ok = keys.set_expiry(database, name(kind), deadline, 0); }
            catch (const std::bad_alloc&) {}
            budget = -1;
            if (ok) {
                ++completed;
                require(source->commits == 1 &&
                    !token->valid(unix_time_ms()), "fault commit count");
                verify(database, kind, deadline);
            } else {
                ++rejected;
                require(!source->commits && root(database, kind) == before &&
                    token->valid(unix_time_ms()), "OOM exposed migration");
            }
        }
        require(completed && rejected, "migration allocation sweep coverage");
        std::cout << "Migration allocation positions: " << kind << " " <<
            count + 1 << '\n';
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
        for (unsigned kind = 0; kind < 5; ++kind) verify(database, kind, -1);
    } else {
        migration(database); failures();
        if (backend == "mdb") { observation(); allocation_failures(); }
    }
    std::cout << "Legacy expiry migration passed: " << backend << '\n';
}
