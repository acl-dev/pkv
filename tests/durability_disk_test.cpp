#include "stdafx.h"
#include "db/page_transaction.h"
#include "coder/string_page.h"
#include "coder/record_codec.h"
#include <chrono>
#include <iostream>
#include <set>
#include <thread>

using namespace pkv;
static const std::string owner("sync\0owner\xff", 11);
static void require(bool ok, const char* reason) {
    if (!ok) { std::cerr << reason << '\n'; std::exit(1); }
}
static shared_db open(const std::string& backend, const std::string& paths) {
    auto database = backend == "rdb" ? db::create_rdb() : db::create_wdb();
    require(database->open(paths.c_str()), "durability disk open");
    return database;
}
static std::string payload(uint64_t epoch, uint64_t id) {
    std::string out(16384, static_cast<char>((epoch + id) % 251));
    for (unsigned i = 0; i < 8; ++i)
        out[i] = static_cast<char>(epoch >> (8 * i));
    return out;
}
static uint64_t number(const std::string& bytes) {
    require(bytes.size() == 16384, "durability page size");
    uint64_t out = 0;
    for (unsigned i = 0; i < 8; ++i)
        out |= uint64_t(static_cast<unsigned char>(bytes[i])) << (8 * i);
    return out;
}
static void seed(shared_db database, const std::string& key, uint64_t epoch) {
    require(database->transact_pages({key}, [&](page_transaction& tx) {
        string_root value;
        value.pages = 2 + epoch % 2; value.length = value.pages * 16384;
        auto& root = *tx.roots().find(key);
        require(string_page_codec::encode_root(value, root.value) ==
            string_page_status::ok && tx.reset_pages(key), "sync root stage");
        root.action = db::update_action::write;
        for (uint64_t i = 1; i <= value.pages; ++i)
            if (!tx.put(key, i, payload(epoch, i))) return false;
        return true;
    }) == db::batch_status::ok, "durability disk epoch commit");
}
static uint64_t check(shared_db database, const std::string& key,
    bool present = true) {
    uint64_t epoch = 0;
    require(database->transact_pages({key}, [&](page_transaction& tx) {
        const auto& root = *tx.roots().find(key);
        require(root.found == present, "durability recovered root existence");
        string_root value;
        if (present) require(string_page_codec::decode_root(root.value, value)
            == string_page_status::ok, "durability recovered root format");
        for (uint64_t i = 1; i <= 4; ++i) {
            std::string bytes;
            const auto found = tx.lookup(key, i, bytes);
            const bool expected = present && i <= value.pages;
            require(found == (expected ? db::lookup_status::found :
                db::lookup_status::missing), "durability recovered page set");
            if (!expected) continue;
            if (i == 1) epoch = number(bytes);
            require(bytes == payload(epoch, i), "torn durability epoch");
        }
        require(!present || (value.pages == 2 + epoch % 2 &&
            value.length == value.pages * 16384), "durability root/page epoch");
        return true;
    }) == db::batch_status::ok, "durability disk check");
    return epoch;
}
static void index(shared_db database, const std::set<std::string>& expected) {
    uint64_t count = 99;
    require(database->key_count(count) == db::count_status::ok &&
        count == expected.size(), "durability root count");
    unsigned long long cursor = 0, next = 0;
    std::set<std::string> actual;
    do {
        std::vector<std::string> keys;
        require(database->scan_keys(cursor, 1, keys, next) ==
            db::scan_status::ok, "durability index scan");
        actual.insert(keys.begin(), keys.end()); cursor = next;
    } while (cursor);
    require(actual == expected, "durability root/page namespace");
}
static void exercise(const std::string& backend, const std::string& paths) {
    std::vector<std::string> keys{"", owner};
    for (unsigned bucket = 0; bucket < 2; ++bucket) {
        std::string key = "partition:" + std::to_string(bucket);
        while (acl_hash_crc32(key.data(), key.size()) % 2 != bucket) key += 'x';
        keys.push_back(key);
    }
    auto database = open(backend, paths);
    require(database->sync_key("missing") == db::sync_status::ok,
        "empty WAL barrier");
    for (size_t i = 0; i < keys.size(); ++i) seed(database, keys[i], i + 1);
    for (unsigned pass = 0; pass < 2; ++pass) {
        for (size_t i = 0; i < keys.size(); ++i) {
            const auto& key = keys[i];
            std::vector<watch_token> watched;
            require(database->watch({key}, watched) == db::batch_status::ok,
                "disk sync WATCH");
            auto waiting = database->wait_for_keys({key});
            std::string before, after;
            require(database->get(key, before) && database->sync_key(key) ==
                db::sync_status::ok && database->get(key, after) &&
                before == after && check(database, key) == i + 1 &&
                watched[0]->valid(unix_time_ms()), "disk barrier changed data");
            key_wait_event event;
            require(!waiting->wait(0, event), "disk barrier published wake");
        }
        index(database, {keys.begin(), keys.end()});
        database.reset(); database = open(backend, paths);
    }
    require(database->del(owner) && database->sync_key(owner) ==
        db::sync_status::ok, "deleted owner WAL barrier");
    database.reset(); database = open(backend, paths);
    check(database, owner, false);
    std::set<std::string> expected(keys.begin(), keys.end());
    expected.erase(owner); index(database, expected);
    std::cout << "Disk WAL barriers, partitions, pages and reopen passed\n";
}
int main(int argc, char** argv) {
    acl::acl_cpp_init();
    require(argc >= 4, "usage: backend mode paths [acknowledged]");
    const std::string backend(argv[1]), mode(argv[2]), paths(argv[3]);
    if (mode == "--exercise") { exercise(backend, paths); return 0; }
    if (mode == "--partial-open") {
        auto candidate = backend == "rdb" ? db::create_rdb() : db::create_wdb();
        require(!candidate->open(paths.c_str()) && candidate->sync_key(owner)
            == db::sync_status::error, "partial initialization synced a WAL");
        std::cout << "Failed engine initialization rejects WAL barriers passed"
            << std::endl;
        return 0;
    }
    auto database = open(backend, paths);
    if (mode == "--writer") {
        for (uint64_t epoch = 1;; ++epoch) {
            seed(database, owner, epoch);
            require(database->sync_key(owner) == db::sync_status::ok,
                "writer WAL barrier");
            std::cout << epoch << std::endl;
        }
    }
    if (mode == "--delete-writer") {
        seed(database, owner, 1);
        require(database->sync_key(owner) == db::sync_status::ok &&
            database->del(owner) && database->sync_key(owner) ==
            db::sync_status::ok, "delete writer barrier");
        std::cout << "deleted" << std::endl;
        for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    if (mode == "--check") {
        require(argc == 5 && check(database, owner) >= std::stoull(argv[4]),
            "acknowledged durable epoch lost");
        index(database, {owner}); return 0;
    }
    require(mode == "--check-deleted", "unknown durability mode");
    check(database, owner, false); index(database, {});
}
