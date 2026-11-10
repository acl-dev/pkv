#include "stdafx.h"
#include "fixture_record_codec.h"
#include "db/list_store.h"
#include "db/mdb/mdb.h"
#include "db/db_watcher.h"
#include "dao/json/json_key.h"
#include "dao/json/json_string.h"
#include "dao/json/json_scan.h"
#include <cstdlib>
#include <iostream>

using namespace pkv;
static const long long deadline = 4102444800123LL;

static void require(bool ok, const char* message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}

class tracking_db : public mdb {
public:
    tracking_db() : mdb(4) {}
    unsigned reads = 0;
    unsigned writes = 0;
    bool fail_metadata = false;
    bool pages_enabled = true;
protected:
    bool dbsupports_pages() const override { return pages_enabled; }
    lookup_status dblookup_page(const std::string& owner, uint64_t id,
        std::string& out) override {
        ++reads;
        return mdb::dblookup_page(owner, id, out);
    }
    batch_status dbapply_pages(const std::vector<batch_entry>& roots,
        const std::vector<page_change>& pages) override {
        if (fail_metadata && pages.empty()) return batch_status::error;
        writes += static_cast<unsigned>(pages.size());
        return mdb::dbapply_pages(roots, pages);
    }
};

class observer : public db_watcher {
public:
    unsigned groups = 0;
    unsigned sets = 0;
    unsigned deletes = 0;
    std::string root;
    bool supports_pages() const override { return true; }
    bool on_pages_database(unsigned database,
        const std::vector<db::batch_entry>& roots,
        const std::vector<page_change>& pages) override {
        require(database == 7 && roots.size() == 1 && pages.empty(),
            "metadata update emitted unexpected pages/DB");
        require(roots[0].action == db::update_action::write &&
            !roots[0].clear_pages, "metadata update cleared page family");
        ++groups;
        root = roots[0].value;
        return true;
    }
    bool on_set(const std::string&, const std::string&, bool ok) override {
        require(ok, "failed observed SET"); ++sets; return true;
    }
    bool on_get(const std::string&, const std::string&, bool) override {
        return true;
    }
    bool on_del(const std::string&, bool ok) override {
        require(ok, "failed observed DEL"); ++deletes; return true;
    }
    bool on_set_database(unsigned, const std::string& key,
        const std::string& value, bool ok) override {
        return on_set(key, value, ok);
    }
    bool on_del_database(unsigned, const std::string& key, bool ok) override {
        return on_del(key, ok);
    }
};

static shared_db memory() {
    auto database = std::make_shared<tracking_db>();
    require(database->open(""), "open");
    return database;
}

static std::vector<std::string> values() {
    return {"", std::string("a\0b", 3), std::string(40000, 'x'), "tail"};
}

static void seed(shared_db database, const std::string& key,
    long long expires = -1) {
    require(database->transact_pages({key}, [&](page_transaction& view) {
        list_store list(view, key);
        list.initialize(expires);
        return list.push(values(), false) && list.finish();
    }) == db::batch_status::ok, "seed");
}

static void check(shared_db database, const std::string& key,
    long long expires) {
    require(database->transact_pages({key}, [&](page_transaction& view) {
        list_store list(view, key);
        std::vector<std::string> out;
        return list.open() && list.metadata().expires_at_ms == expires &&
            list.range(0, -1, out) && out == values() && list.finish();
    }) == db::batch_status::ok, "metadata update lost members");
}

static void absent_pages(shared_db database, const std::string& key) {
    require(database->transact_pages({key}, [&](page_transaction& view) {
        std::string bytes;
        return view.lookup(key, 1, bytes) == db::lookup_status::missing;
    }) == db::batch_status::ok, "old family survived replacement");
}

static void test_view() {
    fixture_record_codec codec;
    list_root root;
    root.elements = 3; root.head = 1; root.tail = 2;
    root.expires_at_ms = deadline;
    for (uint64_t counter : {uint64_t(0), uint64_t(2)}) {
        root.last_id = counter;
        std::string bytes;
        std::unique_ptr<record_view> view;
        require(list_page_codec::encode_root(root, bytes) ==
            list_page_status::ok && codec.decode_stored(bytes, view) ==
            codec_status::ok, "root view decode");
        size_t count = 0;
        std::vector<std::string> payload = {"sentinel"};
        require(view->paged_payload() && !view->valid_payload() &&
            view->header().kind == value_kind::list &&
            view->header().expires_at_ms == deadline &&
            view->list_size(count) && count == 3 &&
            !view->list_values(payload) && payload.size() == 1,
            "root view transferred opaque payload");
        bytes[3] = 4;
        require(codec.decode_stored(bytes, view) ==
            codec_status::unsupported_version && !view, "future root");
    }
    for (const auto& bytes : {std::string("PLR"), std::string("PLR\2", 4),
        std::string("PLR\3", 4)}) {
        std::unique_ptr<record_view> view;
        require(codec.decode_stored(bytes, view) ==
            codec_status::invalid_record && !view, "truncated root");
    }
    root = list_root();
    std::string bytes;
    std::unique_ptr<record_view> view;
    require(list_page_codec::encode_root(root, bytes) == list_page_status::ok &&
        codec.decode_stored(bytes, view) == codec_status::invalid_record,
        "empty stored List accepted");
}

static void test_metadata() {
    shared_db database = memory();
    seed(database, "list");
    auto native = std::static_pointer_cast<tracking_db>(database);
    observer watcher;
    db_watchers watchers(7);
    watchers.add_watcher(&watcher);
    require(database->open("", &watchers), "attach observer");
    dao::json_key keys;
    dao::json_string strings;
    dao::json_scan scan;
    long long result;
    std::string text;
    native->reads = native->writes = 0;
    require(keys.type(database, "list", text) && text == "list" &&
        keys.exists(database, {"list", "missing", "list"}, result) &&
        result == 2 && keys.pttl(database, "list", result) && result == -1,
        "paged TYPE/EXISTS/PTTL");
    require(!strings.get(database, "list", text) && strings.status() ==
        dao::dao_base::read_status::wrong_type, "paged GET wrong type");
    std::vector<dao::string_result> out;
    require(strings.mget(database, {"list", "missing"}, out) &&
        out.size() == 2 && !out[0].found && !out[1].found, "paged MGET");
    scan_options options;
    options.has_type = true; options.type = "list"; options.count = 10000;
    scan_page page;
    require(scan.keys(database, options, page) && page.values.size() == 1 &&
        page.values.front() == "list", "SCAN TYPE list");
    require(keys.set_expiry(database, "list", deadline, 0) &&
        keys.expiry_time(database, "list", result) && result == deadline &&
        watcher.groups == 1, "paged expiry metadata");
    require(!keys.set_expiry(database, "list", deadline + 1,
        dao::key::expire_nx) && watcher.groups == 1, "EXPIRE NX");
    require(!keys.set_expiry(database, "list", deadline - 1,
        dao::key::expire_gt) && watcher.groups == 1, "EXPIRE GT");
    require(keys.set_expiry(database, "list", deadline + 1,
        dao::key::expire_xx | dao::key::expire_gt), "EXPIRE XX GT");
    require(keys.persist(database, "list") &&
        !keys.persist(database, "list") && watcher.groups == 3,
        "paged PERSIST");
    require(native->reads == 0 && native->writes == 0 && watcher.sets == 0,
        "metadata path touched page values or legacy SET");
    check(database, "list", -1);
    bool copied, renamed;
    shared_db transfer = memory();
    seed(transfer, "list");
    require(keys.copy(transfer, "list", transfer, "copy", true, copied) &&
        copied && keys.rename(transfer, "copy", "renamed", false,
            renamed) && renamed, "page-aware key transfer");
    check(database, "list", -1);
    check(transfer, "renamed", -1);
    require(!transfer->get("copy", text), "rename retained source root");
    require(keys.set_expiry(database, "list", 0, 0) &&
        !keys.type(database, "list", text) && watcher.deletes == 1,
        "immediate expiration");
    absent_pages(database, "list");
}

static void test_replacement() {
    shared_db database = memory();
    dao::json_key keys;
    dao::json_string strings;
    seed(database, "list", deadline);
    dao::set_options options;
    options.when = dao::set_options::only_missing;
    dao::set_result result;
    require(strings.set(database, "list", "new", options, result) &&
        !result.applied, "SET NX treated paged root as missing");
    check(database, "list", deadline);
    options.when = dao::set_options::only_existing;
    options.keep_ttl = true;
    require(strings.set(database, "list", "new", options, result) &&
        result.applied, "SET XX KEEPTTL paged replacement");
    absent_pages(database, "list");
    long long expires;
    std::string text;
    require(strings.get(database, "list", text) && text == "new" &&
        keys.expiry_time(database, "list", expires) && expires == deadline,
        "replacement lost TTL");
    seed(database, "list", 0);
    require(!keys.type(database, "list", text) && keys.status() ==
        dao::dao_base::read_status::missing, "lazy expiry");
    absent_pages(database, "list");
    seed(database, "list");
    long long removed;
    require(keys.del(database, {"list", "list"}, removed) && removed == 1,
        "paged multi DEL");
    absent_pages(database, "list");
}

static void test_corruption() {
    shared_db database = memory();
    dao::json_key keys;
    for (int kind = 0; kind < 3; ++kind) {
        seed(database, "list");
        std::string original;
        require(database->get("list", original), "corrupt fixture");
        if (kind == 0) original[3] = 3;
        else if (kind == 1) original[4] = 1;
        else original.resize(4);
        // Preserve real pages while simulating a corrupt/future metadata root.
        require(database->update_root("list", [&](bool, std::string& bytes,
            bool& preserve) {
            bytes = original; preserve = true;
            return db::update_action::write;
        }), "corrupt fixture write");
        std::string text;
        long long count;
        require(!keys.set_expiry(database, "list", deadline, 0) &&
            !keys.persist(database, "list") &&
            !keys.del(database, {"list"}, count) && count == 0 &&
            database->get("list", text) && text == original,
            "invalid root silently rewritten/deleted");
        require(database->transact_pages({"list"}, [](page_transaction& tx) {
            std::string bytes;
            return tx.lookup("list", 1, bytes) == db::lookup_status::found;
        }) == db::batch_status::ok, "invalid root lost pages");
    }
}

class legacy_observer : public observer {
public:
    bool supports_pages() const override { return false; }
};

static void test_failed_preservation() {
    dao::json_key keys;
    for (int failure = 0; failure < 3; ++failure) {
        shared_db database = memory();
        auto native = std::static_pointer_cast<tracking_db>(database);
        seed(database, "list", deadline);
        std::string before, after;
        require(database->get("list", before), "failure root fixture");
        legacy_observer watcher;
        db_watchers watchers;
        watchers.add_watcher(&watcher);
        if (failure == 0) native->fail_metadata = true;
        if (failure == 1) {
            require(database->open("", &watchers), "legacy observer attach");
        }
        if (failure == 2) native->pages_enabled = false;
        native->reads = native->writes = 0;
        require(!keys.set_expiry(database, "list", deadline + 1, 0) &&
            keys.status() == dao::dao_base::read_status::storage_error &&
            !keys.persist(database, "list") &&
            database->get("list", after) && after == before &&
            !native->reads && !native->writes && !watcher.groups,
            "failed metadata preservation changed data or reported success");
        native->fail_metadata = false;
        native->pages_enabled = true;
        check(database, "list", deadline);
    }
}

int main() {
    test_view();
    test_metadata();
    test_replacement();
    test_corruption();
    test_failed_preservation();
    std::cout << "paged key metadata tests passed\n";
}
