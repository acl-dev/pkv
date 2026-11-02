#include "stdafx.h"
#include "db/db.h"
#include "db/page_transaction.h"
#include "db/rocksdb/rdb.h"
#include "dao/paged_list.h"
#include "dao/paged_hash.h"
#include "dao/paged_set.h"
#include "dao/paged_zset.h"
#include "dao/json/json_key.h"
#include <rocksdb/db.h>
#include <rocksdb/write_batch.h>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <unistd.h>

using namespace pkv;
static void require(bool ok, const char* reason) {
    if (!ok) { std::cerr << reason << '\n'; std::exit(1); }
}
struct directory {
    std::string path;
    directory() {
        char name[] = "/tmp/pkv-rocks-test-XXXXXX";
        require(::mkdtemp(name) != nullptr, "mkdtemp"); path = name;
    }
    ~directory() {
        rocksdb::DestroyDB(path + "/rdb", rocksdb::Options());
        ::rmdir(path.c_str());
    }
};
static shared_db open(const std::string& path) {
    auto database = db::create_rdb();
    require(database && database->open(path.c_str()), "RocksDB open");
    return database;
}
static std::set<std::string> keys(shared_db database) {
    std::set<std::string> out;
    unsigned long long cursor = 0, next;
    do {
        std::vector<std::string> page;
        require(database->scan_keys(cursor, 1, page, next) ==
            db::scan_status::ok, "SCAN");
        out.insert(page.begin(), page.end()); cursor = next;
    } while (cursor);
    return out;
}
struct raw_storage {
    rocksdb::DB* database = nullptr;
    std::vector<rocksdb::ColumnFamilyHandle*> handles;
    explicit raw_storage(const std::string& path) {
        std::vector<std::string> names;
        require(rocksdb::DB::ListColumnFamilies(rocksdb::DBOptions(), path,
            &names).ok(), "list column families");
        std::vector<rocksdb::ColumnFamilyDescriptor> descriptors;
        for (const auto& name : names)
            descriptors.emplace_back(name, rocksdb::ColumnFamilyOptions());
        require(rocksdb::DB::Open(rocksdb::DBOptions(), path, descriptors,
            &handles, &database).ok(), "raw column-family open");
    }
    ~raw_storage() {
        for (auto* handle : handles) delete handle;
        delete database;
    }
    rocksdb::ColumnFamilyHandle* family(const std::string& name) {
        for (auto* handle : handles)
            if (handle->GetName() == name) return handle;
        require(false, "missing column family"); return nullptr;
    }
};

static void test_legacy_and_private_namespaces() {
    directory home;
    rocksdb::Options options; options.create_if_missing = true;
    rocksdb::DB* raw = nullptr;
    require(rocksdb::DB::Open(options, home.path + "/rdb", &raw).ok(),
        "legacy open");
    const std::string binary("\0\xffowner\0", 8);
    const std::string address = std::string(15, '\0') + '\1';
    const std::string payload("\0\xff\0", 3);
    require(raw->Put(rocksdb::WriteOptions(), "", "empty").ok() &&
        raw->Put(rocksdb::WriteOptions(), binary, payload).ok() &&
        raw->Put(rocksdb::WriteOptions(), address, "collision").ok() &&
        raw->Put(rocksdb::WriteOptions(), "pkv.pages.v1", "ordinary").ok(),
        "legacy roots");
    rocksdb::ColumnFamilyHandle* extra = nullptr;
    require(raw->CreateColumnFamily(rocksdb::ColumnFamilyOptions(), "other",
        &extra).ok() && raw->Put(rocksdb::WriteOptions(), extra, "keep",
            "untouched").ok(), "foreign column family");
    delete extra; delete raw;
    auto database = open(home.path);
    const std::set<std::string> expected = {"", binary, address,
        "pkv.pages.v1"};
    require(keys(database) == expected, "legacy SCAN index rebuild");
    std::string original = "sentinel";
    require(database->get(binary, original) && original == payload &&
        !database->get("missing", original) && original == payload,
        "binary payload/missing lookup");
    require(database->transact_pages({"", binary}, [&](page_transaction& tx) {
        for (const auto& key : std::vector<std::string>{"", binary}) {
            auto& root = *tx.roots().find(key);
            root.value = "paged"; root.action = db::update_action::write;
            if (!tx.put(key, 1, std::string(16384, 'x')) ||
                !tx.put(key, UINT64_MAX, "tail")) return false;
        }
        return true;
    }) == db::batch_status::ok, "atomic binary page owners");
    require(keys(database) == expected, "private page leaked into SCAN");
    require(database->set("", "replacement"), "overwrite");
    require(database->transact_pages({"", binary}, [&](page_transaction& tx) {
        std::string bytes;
        return tx.lookup("", UINT64_MAX, bytes) ==
                db::lookup_status::missing &&
            tx.lookup("", 1, bytes) == db::lookup_status::missing &&
            tx.lookup(binary, 1, bytes) == db::lookup_status::found &&
            bytes == std::string(16384, 'x');
    }) == db::batch_status::ok, "raw overwrite page ownership");
    database.reset(); database = open(home.path);
    require(keys(database) == expected, "reopen SCAN index");
    std::string bytes;
    require(database->get("", bytes) && bytes == "replacement", "reopen root");
    require(database->del(binary) && keys(database) ==
        std::set<std::string>({"", address, "pkv.pages.v1"}), "delete index");
    require(database->transact_pages({binary}, [&](page_transaction& tx) {
        return tx.lookup(binary, 1, bytes) == db::lookup_status::missing &&
            tx.lookup(binary, UINT64_MAX, bytes) == db::lookup_status::missing;
    }) == db::batch_status::ok, "delete left orphan pages");
    database.reset();
    {
        raw_storage storage(home.path + "/rdb");
        std::string value;
        require(storage.database->Get(rocksdb::ReadOptions(),
            storage.family("other"), "keep", &value).ok() &&
            value == "untouched", "foreign column family changed");
        require(storage.database->Get(rocksdb::ReadOptions(),
            storage.family("pkv.pages.v1"), "", &value).ok() &&
            value == "scan-v1", "empty owner reset deleted schema marker");
        require(storage.database->Delete(rocksdb::WriteOptions(),
            storage.family("pkv.pages.v1"), "").ok() &&
            storage.database->Put(rocksdb::WriteOptions(),
                storage.family("pkv.scan.v1"), "ghost", "").ok(),
            "incomplete index rebuild fixture");
    }
    database = open(home.path);
    require(keys(database) == std::set<std::string>({"", address,
        "pkv.pages.v1"}), "incomplete index rebuild retained ghost keys");
    require(database->get(address, bytes) && bytes == "collision",
        "private page address collided with user key");
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
        "native List");
    require(hash.hmset(database, "hash", {{"", large}, {"count", "1"}}) == 2,
        "native Hash");
    require(set.change(database, "set", {"", large}, true, count) && count == 2,
        "native Set");
    dao::zadd_result added;
    require(sorted.add(database, "zset", {{"", -0.0}, {large, 2}}, {}, added)
        && added.added == 2, "native ZSet");
    dao::json_key key;
    bool copied;
    require(key.rename(database, "hash", "renamed", false, copied) && copied,
        "same-instance atomic rename");
    require(key.set_expiry(database, "zset", 4102444800123LL, 0), "TTL");
    database.reset();
    {
        raw_storage storage(home.path + "/rdb");
        std::unique_ptr<rocksdb::Iterator> pages(storage.database->NewIterator(
            rocksdb::ReadOptions(), storage.family("pkv.pages.v1")));
        unsigned count = 0;
        for (pages->SeekToFirst(); pages->Valid(); pages->Next(), ++count)
            require(pages->value().size() <= 16384, "unbounded disk page");
        require(pages->status().ok() && count > 8,
            "missing physical fragments");
        std::unique_ptr<rocksdb::Iterator> roots(storage.database->NewIterator(
            rocksdb::ReadOptions(), storage.family("default")));
        for (roots->SeekToFirst(); roots->Valid(); roots->Next())
            require(roots->value().size() <= 64, "whole collection disk root");
        require(roots->status().ok(), "physical root iterator");
    }
    database = open(home.path);
    std::vector<std::string> members;
    require(list.range(database, "list", 0, -1, members) &&
        members == std::vector<std::string>({"", large}), "persistent List");
    std::string value;
    require(hash.hget(database, "renamed", "", value) && value == large,
        "persistent Hash");
    std::set<std::string> values;
    require(set.members(database, "set", values) && values ==
        std::set<std::string>({"", large}), "persistent Set");
    std::vector<zset_score> scores;
    require(sorted.scores(database, "zset", {"", large}, scores) &&
        scores.size() == 2 && scores[0].found && std::signbit(scores[0].score)
        && scores[1].score == 2 && key.expiry_time(database, "zset", count) &&
        count == 4102444800123LL, "persistent ZSet/TTL/signed zero");
    require(keys(database) == std::set<std::string>({"list", "renamed", "set",
        "zset"}), "persistent logical key namespace");
}

static void test_cross_shard_preflight() {
    directory left, right;
    auto database = open(left.path + ";" + right.path);
    const auto shard = [](const std::string& key) {
        return acl_hash_crc32(key.data(), key.size()) % 2;
    };
    std::string a = "a", b = "b";
    while (shard(a) == shard(b)) b += "x";
    require(database->set(a, "old-a") && database->set(b, "old-b"), "seed");
    require(database->transact_pages({a, b}, [&](page_transaction& tx) {
        for (const auto& key : std::vector<std::string>{a, b}) {
            auto& root = *tx.roots().find(key);
            root.value = "new"; root.action = db::update_action::write;
            if (!tx.put(key, 1, "new-page")) return false;
        }
        return true;
    }) == db::batch_status::unsupported, "cross-shard transaction accepted");
    std::string value;
    require(database->get(a, value) && value == "old-a" &&
        database->get(b, value) && value == "old-b",
        "partial cross-shard write");
    require(database->transact_pages({a, b}, [&](page_transaction& tx) {
        return tx.lookup(a, 1, value) == db::lookup_status::missing &&
            tx.lookup(b, 1, value) == db::lookup_status::missing;
    }) == db::batch_status::ok, "cross-shard preflight leaked pages");
    require(database->transact_pages({a, b}, [&](page_transaction& tx) {
        auto& root = *tx.roots().find(a);
        root.action = db::update_action::write; root.value = "only-a";
        return tx.put(a, 1, "one-instance");
    }) == db::batch_status::ok && database->get(a, value) && value == "only-a"
        && database->get(b, value) && value == "old-b",
        "read-only cross-shard input was unnecessarily rejected");
    require(keys(database) == std::set<std::string>({a, b}), "multi-path SCAN");
}

static bool epoch(shared_db database, unsigned value) {
    const std::string marker = std::to_string(value);
    return database->transact_pages({"left", "right"},
        [&](page_transaction& tx) {
        for (const auto& key : {"left", "right"}) {
            auto& root = *tx.roots().find(key);
            root.value = marker; root.action = db::update_action::write;
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
    require(database->get("left", marker), "recovered root missing");
    const unsigned recovered = static_cast<unsigned>(std::stoul(marker));
    require(recovered >= acknowledged, "acknowledged WAL update lost");
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
    }) == db::batch_status::ok, "recovery exposed a partial epoch");
    require(keys(database) == std::set<std::string>({"left", "right"}),
        "crash recovery broke index atomicity");
}
int main(int argc, char** argv) {
    acl::acl_cpp_init();
    if (argc == 3 && std::string(argv[1]) == "--writer") writer(argv[2]);
    else if (argc == 4 && std::string(argv[1]) == "--check")
        check(argv[2], static_cast<unsigned>(std::stoul(argv[3])));
    else {
        require(argc == 1, "invalid test arguments");
        test_legacy_and_private_namespaces(); test_types_and_reopen();
        test_cross_shard_preflight();
        std::cout << "RocksDB native pages, legacy keys and reopen passed\n";
    }
}
