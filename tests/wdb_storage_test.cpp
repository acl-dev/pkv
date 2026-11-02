#include "stdafx.h"
#include "db/db.h"
#include "db/page_transaction.h"
#include "dao/paged_list.h"
#include "dao/paged_hash.h"
#include "dao/paged_set.h"
#include "dao/paged_zset.h"
#include "dao/json/json_key.h"
#include <wiredtiger.h>
#include <cmath>
#include <cstdlib>
#include <ftw.h>
#include <iostream>
#include <unistd.h>

using namespace pkv;
static void require(bool ok, const char* reason) {
    if (!ok) { std::cerr << reason << '\n'; std::exit(1); }
}
struct directory {
    std::string path;
    directory() {
        char name[] = "/tmp/pkv-wt-test-XXXXXX";
        require(::mkdtemp(name) != nullptr, "mkdtemp"); path = name;
    }
    static int cleanup(const char* name, const struct stat*, int, struct FTW*) {
        return ::remove(name);
    }
    ~directory() { ::nftw(path.c_str(), cleanup, 64, FTW_DEPTH | FTW_PHYS); }
};
struct raw_storage {
    WT_CONNECTION* connection = nullptr;
    WT_SESSION* session = nullptr;
    explicit raw_storage(const std::string& path) {
        require(wiredtiger_open(path.c_str(), nullptr,
            "create,log=(enabled=true),cache_size=128MB", &connection) == 0 &&
            connection->open_session(connection, nullptr, nullptr, &session)
                == 0, "raw WiredTiger open");
    }
    ~raw_storage() {
        if (session) session->close(session, nullptr);
        if (connection) connection->close(connection, nullptr);
    }
    WT_CURSOR* cursor(const char* table) {
        WT_CURSOR* out = nullptr;
        require(session->open_cursor(session, table, nullptr, nullptr, &out)
            == 0, "raw cursor");
        return out;
    }
    static void key(WT_CURSOR* cursor, const std::string& bytes) {
        WT_ITEM item = {}; item.data = bytes.data(); item.size = bytes.size();
        cursor->set_key(cursor, &item);
    }
};
static shared_db open(const std::string& path) {
    auto database = db::create_wdb();
    require(database && database->open(path.c_str()), "WiredTiger open");
    return database;
}
static std::set<std::string> keys(shared_db database) {
    std::set<std::string> out;
    unsigned long long cursor = 0, next;
    do {
        std::vector<std::string> page;
        require(database->scan_keys(cursor, 1, page, next) ==
            db::scan_status::ok,
            "SCAN");
        out.insert(page.begin(), page.end()); cursor = next;
    } while (cursor);
    return out;
}
static void test_legacy_and_binary() {
    directory home;
    require(::mkdir((home.path + "/wdb").c_str(), 0700) == 0, "mkdir");
    {
        raw_storage old(home.path + "/wdb");
        require(old.session->create(old.session, "table:access",
            "type=lsm,key_format=S,value_format=S") == 0, "legacy LSM table");
        auto* cursor = old.cursor("table:access");
        cursor->set_key(cursor, "plain"); cursor->set_value(cursor, "old");
        require(cursor->insert(cursor) == 0, "legacy insert");
    }
    auto database = open(home.path);
    std::string out;
    require(database->get("plain", out) && out == "old", "legacy migration");
    const std::string binary("a\0b\xff", 4), payload("v\0x\xff", 4);
    const std::string address = std::string(15, '\0') + '\1';
    require(database->set("plain", "new") && database->set("", payload) &&
        database->set(binary, payload) && database->set(address, "user-key"),
        "binary roots");
    const std::set<std::string> expected = {"plain", "", binary, address};
    require(keys(database) == expected, "binary key SCAN");
    require(database->transact_pages({"", binary}, [&](page_transaction& tx) {
        for (const auto& key : std::vector<std::string>{"", binary}) {
            auto& root = *tx.roots().find(key);
            root.value = "paged"; root.action = db::update_action::write;
            if (!tx.put(key, 1, std::string(16384, 'x')) ||
                !tx.put(key, UINT64_MAX, "tail")) return false;
        }
        return true;
    }) == db::batch_status::ok, "binary page owners");
    require(database->set("", "overwrite") && database->get(address, out) &&
        out == "user-key", "page address collided with user key");
    require(database->transact_pages({"", binary}, [&](page_transaction& tx) {
        return tx.lookup("", 1, out) == db::lookup_status::missing &&
            tx.lookup("", UINT64_MAX, out) == db::lookup_status::missing &&
            tx.lookup(binary, 1, out) == db::lookup_status::found &&
            out == std::string(16384, 'x');
    }) == db::batch_status::ok, "owner-range reclamation");
    database.reset();
    {
        raw_storage old(home.path + "/wdb");
        auto* legacy = old.cursor("table:access");
        legacy->set_key(legacy, "plain");
        const char* value;
        require(legacy->search(legacy) == 0 && legacy->get_value(legacy, &value)
            == 0 && std::string(value) == "old", "legacy table modified");
        auto* marker = old.cursor("table:pkv_meta_v1");
        const std::string schema_key = "schema";
        raw_storage::key(marker, schema_key);
        require(marker->remove(marker) == 0, "partial bootstrap fixture");
    }
    database = open(home.path);
    require(database->get("plain", out) && out == "new" &&
        keys(database) == expected, "rebuild overwrote native data");
    require(database->del(binary) && database->transact_pages({binary},
        [&](page_transaction& tx) {
        return tx.lookup(binary, 1, out) == db::lookup_status::missing &&
            tx.lookup(binary, UINT64_MAX, out) == db::lookup_status::missing;
    }) == db::batch_status::ok, "delete page family");
}
static void test_types_and_reopen() {
    directory home;
    auto database = open(home.path);
    dao::paged_list list;
    dao::paged_hash hash;
    dao::paged_set set;
    dao::paged_zset sorted;
    const std::string large(100000, 'm');
    long long count;
    require(list.push(database, "list", {"", large}, false, false, count),
        "List seed");
    require(hash.hmset(database, "hash", {{"", large}, {"count", "1"}}) == 2,
        "Hash seed");
    require(set.change(database, "set", {"", large}, true, count), "Set seed");
    dao::zadd_result added;
    require(sorted.add(database, "zset", {{"", -0.0}, {large, 2}}, {}, added),
        "ZSet seed");
    dao::json_key key;
    bool renamed;
    require(key.rename(database, "hash", "renamed", false, renamed) &&
        renamed && key.set_expiry(database, "zset", 4102444800123LL, 0),
        "rename/TTL");
    database.reset();
    {
        raw_storage raw(home.path + "/wdb");
        auto* pages = raw.cursor("table:pkv_pages_v1");
        unsigned count = 0;
        int result;
        while ((result = pages->next(pages)) == 0) {
            WT_ITEM value = {};
            require(pages->get_value(pages, &value) == 0 && value.size <= 16384,
                "unbounded physical page");
            ++count;
        }
        require(result == WT_NOTFOUND && count > 8, "missing physical pages");
        auto* roots = raw.cursor("table:pkv_roots_v1");
        while ((result = roots->next(roots)) == 0) {
            WT_ITEM value = {};
            require(roots->get_value(roots, &value) == 0 && value.size <= 64,
                "whole collection root");
        }
        require(result == WT_NOTFOUND, "physical root scan");
    }
    database = open(home.path);
    std::vector<std::string> members;
    require(list.range(database, "list", 0, -1, members) &&
        members == std::vector<std::string>({"", large}), "persistent List");
    std::string value;
    require(hash.hget(database, "renamed", "", value) && value == large,
        "persistent Hash");
    std::set<std::string> values;
    require(set.members(database, "set", values) &&
        values == std::set<std::string>({"", large}), "persistent Set");
    std::vector<zset_score> scores;
    require(sorted.scores(database, "zset", {"", large}, scores) &&
        scores[0].found && std::signbit(scores[0].score) && scores[1].score == 2
        && key.expiry_time(database, "zset", count) && count == 4102444800123LL,
        "persistent ZSet/TTL");
}
static void test_cross_shard_preflight() {
    directory a, b;
    auto database = open(a.path + ";" + b.path);
    std::string first = "a", second = "b";
    const auto shard = [](const std::string& key) {
        return acl_hash_crc32(key.data(), key.size()) % 2;
    };
    while (shard(first) == shard(second)) second += "x";
    require(database->set(first, "old-a") && database->set(second, "old-b"),
        "cross-shard seed");
    require(database->transact_pages({first, second},
        [&](page_transaction& tx) {
        for (const auto& key : std::vector<std::string>{first, second}) {
            auto& root = *tx.roots().find(key);
            root.action = db::update_action::write; root.value = "new";
            if (!tx.put(key, 1, "new-page")) return false;
        }
        return true;
    }) == db::batch_status::unsupported, "cross-shard commit accepted");
    std::string value;
    require(database->get(first, value) && value == "old-a" &&
        database->get(second, value) && value == "old-b", "partial commit");
    require(database->transact_pages({first, second},
        [&](page_transaction& tx) {
        return tx.lookup(first, 1, value) == db::lookup_status::missing &&
            tx.lookup(second, 1, value) == db::lookup_status::missing;
    }) == db::batch_status::ok, "cross-shard partial pages");
    require(database->transact_pages({first, second},
        [&](page_transaction& tx) {
        if (tx.roots().find(second)->value != "old-b") return false;
        auto& root = *tx.roots().find(first);
        root.action = db::update_action::write; root.value = "new-a";
        return tx.put(first, 1, "new-page");
    }) == db::batch_status::ok, "cross-shard read-only input rejected");
    require(database->get(first, value) && value == "new-a" &&
        database->get(second, value) && value == "old-b",
        "read-only input changed");
    require(keys(database) == std::set<std::string>({first, second}),
        "cross-shard scan");
}
static bool epoch(shared_db database, unsigned value) {
    const auto marker = std::to_string(value);
    return database->transact_pages({"left", "right"},
        [&](page_transaction& tx) {
        for (const auto& key : {"left", "right"}) {
            auto& root = *tx.roots().find(key);
            root.action = db::update_action::write; root.value = marker;
            for (uint64_t id = 1; id <= 32; ++id)
                if (!tx.put(key, id, marker + std::string(16000, 'x')))
                    return false;
        }
        return true;
    }) == db::batch_status::ok;
}
static void writer(const std::string& path) {
    auto database = open(path);
    for (unsigned value = 1; ; ++value) {
        require(epoch(database, value), "epoch commit");
        std::cout << value << std::endl;
    }
}
static void check(const std::string& path, unsigned acknowledged) {
    auto database = open(path);
    std::string marker;
    require(database->get("left", marker) &&
        std::stoul(marker) >= acknowledged, "acknowledged commit lost");
    require(database->transact_pages({"left", "right"},
        [&](page_transaction& tx) {
        for (const auto& key : {"left", "right"}) {
            if (tx.roots().find(key)->value != marker) return false;
            for (uint64_t id = 1; id <= 32; ++id) {
                std::string value;
                if (tx.lookup(key, id, value) != db::lookup_status::found ||
                    value != marker + std::string(16000, 'x')) return false;
            }
        }
        return true;
    }) == db::batch_status::ok, "partial recovered transaction");
    require(keys(database) == std::set<std::string>({"left", "right"}),
        "recovered index mismatch");
}
int main(int argc, char** argv) {
    acl::acl_cpp_init();
    if (argc == 3 && std::string(argv[1]) == "--writer") writer(argv[2]);
    else if (argc == 4 && std::string(argv[1]) == "--check")
        check(argv[2], static_cast<unsigned>(std::stoul(argv[3])));
    else {
        require(argc == 1, "invalid test arguments");
        test_legacy_and_binary(); test_types_and_reopen();
        test_cross_shard_preflight();
        std::cout << "WiredTiger binary pages, migration and reopen passed\n";
    }
}
