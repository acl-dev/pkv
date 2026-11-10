#include "stdafx.h"
#include "native_string_fixture.h"
#include "dao/json/json_string.h"
#include "dao/json/json_key.h"
#include "dao/json/json_hash.h"
#include "dao/json/json_list.h"
#include "dao/json/json_set.h"
#include "dao/json/json_scan.h"
#include "dao/json/json_zset.h"
#include <limits>
#include "common/set_sampling.h"
#include "common/redis_integer.h"
#include <stdexcept>
#include <atomic>
#include "db/db_cursor.h"
#include "db/db_watcher.h"
#include "db/database_pool.h"
#include "cluster/cluster_client.h"
#include <cstdlib>
#include <iostream>

static void require(bool ok, const char* message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}

// Inspect exact legacy records and errors independently of any disk backend.
class fixture_db : public pkv::db {
public:
    std::unordered_map<std::string, std::string> records;
    bool fail_write = false;
    bool fail_read = false;
    bool fail_delete = false;
    lookup_status dblookup(const std::string& key, std::string& value) override {
        if (fail_read) return lookup_status::error;
        return db::dblookup(key, value);
    }
    bool dbopen(const char*) override { return true; }
    bool dbset(const std::string& key, const std::string& value) override {
        if (fail_write) return false;
        records[key] = value;
        return true;
    }
    bool dbget(const std::string& key, std::string& value) override {
        auto it = records.find(key);
        if (it == records.end()) return false;
        value = it->second;
        return true;
    }
    bool dbdel(const std::string& key) override { return !fail_delete && records.erase(key) != 0; }
    pkv::db_cursor* dbcreate_cursor() override { return nullptr; }
    bool dbscan(size_t, pkv::db_cursor&, std::vector<std::string>&, size_t) override { return false; }
    size_t dbsize() const override { return 1; }
    const char* get_dbtype() const override { return "fixture"; }
};

class batch_fixture_db : public fixture_db {
public:
    bool fail_batch = false;
    batch_status dbapply(const std::vector<batch_entry>& entries) override {
        if (fail_batch) return batch_status::error;
        auto staged = records;
        for (const auto& entry : entries) {
            if (entry.action == update_action::write) staged[entry.key] = entry.value;
            else if (entry.action == update_action::erase) staged.erase(entry.key);
        }
        records.swap(staged);
        return batch_status::ok;
    }
};

class counting_watcher : public pkv::db_watcher {
public:
    int sets = 0, deletions = 0;
    bool on_set(const std::string&, const std::string&, bool ok) override { if (ok) ++sets; return true; }
    bool on_del(const std::string&, bool ok) override { if (ok) ++deletions; return true; }
    bool on_get(const std::string&, const std::string&, bool) override { return true; }
};

class database_counter : public counting_watcher {
public:
    unsigned last_database = 0;
    bool on_set_database(unsigned database, const std::string& key, const std::string& value, bool ok) override {
        last_database = database;
        return on_set(key, value, ok);
    }
    bool on_del_database(unsigned database, const std::string& key, bool ok) override {
        last_database = database;
        return on_del(key, ok);
    }
};

class rejecting_codec : public pkv::record_codec {
public:
    mutable int calls = 0;
    int fail_at = -1;
    pkv::codec_status decode(const std::string& bytes, std::unique_ptr<pkv::record_view>& out) const override {
        return pkv::default_record_codec()->decode(bytes, out);
    }
    pkv::codec_status encode(const pkv::value_record& value, std::string& out) const override {
        if (++calls == fail_at) return pkv::codec_status::out_of_memory;
        return pkv::default_record_codec()->encode(value, out);
    }
};

class scan_fixture_db : public fixture_db {
public:
    bool fail_scan = false;
    scan_status dbscan_keys(unsigned long long, size_t, std::vector<std::string>& keys, unsigned long long& next) override {
        if (fail_scan) return scan_status::error;
        next = 0;
        for (const auto& record : records) keys.push_back(record.first);
        return scan_status::ok;
    }
};

static std::string fixture_value(pkv::value_kind kind,
    const std::string& value, long long deadline = -1) {
    pkv::value_record record;
    record.header.kind = kind;
    record.header.expires_at_ms = deadline;
    if (kind == pkv::value_kind::string) record.string_value = value;
    if (kind == pkv::value_kind::list) record.list_value = {value};
    if (kind == pkv::value_kind::set) record.set_value = {value};
    std::string bytes;
    require(pkv::default_record_codec()->encode(record, bytes) ==
        pkv::codec_status::ok, "fixture encoding failed");
    return bytes;
}

int main() {
    acl::acl_cpp_init();
    using status = pkv::dao::dao_base::read_status;
    auto fixture = std::make_shared<fixture_db>();
    pkv::shared_db db = fixture;
    pkv::dao::json_string strings;
    pkv::dao::json_key keys;
    std::string out;
    fixture->records["legacy"] = fixture_value(pkv::value_kind::string, "hello");
    require(strings.get(db, "legacy", out) && out == "hello", "legacy record unreadable");
    fixture->records["empty"] = fixture_value(pkv::value_kind::string, "");
    require(strings.get(db, "empty", out) && out.empty(), "empty record lost");
    require(!strings.get(db, "absent", out) && strings.status() == status::missing, "missing status incorrect");
    fixture->records["hash"] = fixture_value(pkv::value_kind::hash, "");
    require(!strings.get(db, "hash", out) && strings.status() == status::wrong_type, "wrongtype status incorrect");
    const std::vector<std::string> malformed = {
        "not JSON", "[]", "{}",
        "{\"T\":\"S\",\"E\":18446744073709551615,\"D\":\"x\"}",
        "{\"T\":\"S\",\"E\":-2,\"D\":\"x\"}", "{\"T\":\"S\",\"E\":-1}",
        "{\"T\":\"S\",\"E\":\"no\",\"D\":\"x\"}",
        "{\"T\":\"S\",\"E\":-1,\"D\":{}}"
    };
    for (const auto& record : malformed) {
        fixture->records["bad"] = record;
        require(!strings.get(db, "bad", out) && strings.status() == status::invalid_record, "corruption status incorrect");
        require(fixture->records["bad"] == record, "read destroyed a corrupted record");
    }
    fixture->records["expired"] = fixture_value(pkv::value_kind::string, "old", 0);
    require(!strings.get(db, "expired", out) && strings.status() == status::missing, "expired key visible");
    require(!fixture->records.count("expired"), "expired key retained");
    // Reuse a DAO repeatedly to catch stale writer state and reader ownership.
    std::string binary;
    for (int byte = 0; byte < 256; ++byte) binary.push_back(static_cast<char>(byte));
    for (int i = 0; i < 500; ++i) {
        const auto value = binary + std::to_string(i);
        require(strings.set(db, "reuse", value) && strings.get(db, "reuse", out) && out == value,
                "reused DAO truncated or returned stale bytes");
    }
    require(strings.set(db, "binary-expiry", binary), "binary set failed");
    require(keys.expire(db, "binary-expiry", 60), "expiry rewrite rejected binary data");
    require(strings.get(db, "binary-expiry", out) && out == binary, "expiry rewrite lost binary data");
    fixture->fail_write = true;
    require(!strings.set(db, "write-failure", std::string("x")), "write failure hidden");
    // The built-in multithreaded map must preserve storage bytes too.
    auto memory = pkv::db::create_mdb(16);
    require(memory->open(""), "memory open failed");
    require(memory->set(binary, binary) && memory->get(binary, out) && out == binary, "memory backend truncated bytes");
    fixture->fail_write = false;
    pkv::dao::json_hash hashes;
    require(hashes.hmset(db, "fields", {{"a", "one"}, {"b", "two"}}) == 2, "hash creation count");
    require(hashes.hmset(db, "fields", {{"a", "updated"}}) == 0, "hash overwrite count");
    require(hashes.hget(db, "fields", "a", out) && out == "updated", "hash reused stale writer");
    require(hashes.hmset(db, binary, {{binary, binary}, {"", ""}}) == 2, "binary hash set");
    require(hashes.hget(db, binary, binary, out) && out == binary, "binary hash truncated");
    require(hashes.hget(db, binary, "", out) && out.empty(), "empty hash value missing");
    require(keys.expire(db, "fields", 60), "hash expire failed");
    require(hashes.hmset(db, "fields", {{"c", "three"}}) == 1, "hash extend failed");
    long long ttl;
    require(keys.ttl(db, "fields", ttl) && ttl >= 59 && ttl <= 60, "hash update cleared TTL");
    require(hashes.hdel(db, "fields", std::vector<std::string>{"a", "a", "none"}) == 1, "hash delete duplicate count");
    require(keys.ttl(db, "fields", ttl) && ttl >= 59 && ttl <= 60, "hash delete cleared TTL");
    fixture->records["protected"] = fixture_value(pkv::value_kind::string, "safe");
    const auto original = fixture->records["protected"];
    require(hashes.hmset(db, "protected", {{"bad", "bad"}}) == -1 && hashes.status() == status::wrong_type,
            "hash overwrote a string");
    require(fixture->records["protected"] == original, "wrongtype changed stored data");
    fixture->records["bad-hash"] = "{\"T\":\"H\",\"E\":-1,\"D\":{\"a\":5}}";
    const auto corrupt = fixture->records["bad-hash"];
    require(hashes.hmset(db, "bad-hash", {{"a", "bad"}}) == -1 && hashes.status() == status::invalid_record,
            "hash accepted corrupt field");
    require(fixture->records["bad-hash"] == corrupt, "hash destroyed corruption evidence");
    fixture->fail_read = true;
    require(!strings.get(db, "protected", out) && strings.status() == status::storage_error, "I/O error became missing");
    require(hashes.hmset(db, "protected", {{"bad", "bad"}}) == -1 && hashes.status() == status::storage_error,
            "failed read allowed a write");
    fixture->fail_read = false;
    fixture->fail_delete = true;
    require(hashes.hdel(db, binary, std::vector<std::string>{binary, ""}) == -1, "failed delete reported success");
    fixture->fail_delete = false;
    require(keys.expire(db, "protected", 0) && !fixture->records.count("protected"), "zero expiry not deleted immediately");
    for (const auto& text : {"+1", "01", "-0", " 1", "9223372036854775808"}) {
        long long integer;
        require(!pkv::parse_redis_integer(text, integer), "non-canonical integer accepted");
    }
    // Many OS threads must not lose independent fields in the same hash.
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) threads.emplace_back([&, t] {
        pkv::dao::json_hash writer;
        for (int i = 0; i < 100; ++i) {
            auto name = std::to_string(t) + ":" + std::to_string(i);
            require(writer.hmset(memory, "shared-hash", {{name, binary}}) == 1, "concurrent hash write failed");
        }
    });
    for (auto& thread : threads) thread.join();
    require(hashes.hgetall(memory, "shared-hash") && hashes.get_fields().size() == 400, "concurrent hash fields lost");
    threads.clear();
    // Yield while holding a key lock: waiting fibers must release the worker
    // thread so that its lock owner can resume, including across OS threads.
    require(memory->set("counter", "0"), "counter setup");
    std::atomic<int> ready{0};
    for (int t = 0; t < 4; ++t) threads.emplace_back([&] {
        ++ready;
        while (ready.load() != 4) std::this_thread::yield();
        for (int f = 0; f < 8; ++f) go[&] {
            for (int i = 0; i < 20; ++i) {
                require(memory->update("counter", [](bool found, std::string& value) {
                    require(found, "counter missing");
                    const long long n = std::stoll(value);
                    acl::fiber::yield();
                    value = std::to_string(n + 1);
                    return pkv::db::update_action::write;
                }), "yielding update failed");
            }
        };
        acl::fiber::schedule();
    });
    for (auto& thread : threads) thread.join();
    require(memory->get("counter", out) && out == "640", "yielding updates lost or deadlocked");
    try {
        memory->update("counter", [](bool, std::string&) -> pkv::db::update_action {
            throw std::runtime_error("test exception");
        });
        require(false, "callback exception disappeared");
    } catch (const std::runtime_error&) {}
    require(memory->get("counter", out) && out == "640", "callback exception kept a lock");
    bool applied;
    require(!strings.mset(db, {{"unsupported-a", "a"}, {"unsupported-b", "b"}}, false, applied) &&
            strings.status() == status::unsupported_operation && !applied,
            "unsupported batch silently performed sequential writes");
    require(!fixture->records.count("unsupported-a") && !fixture->records.count("unsupported-b"), "unsupported batch wrote values");
    auto batch_fixture = std::make_shared<batch_fixture_db>();
    pkv::shared_db batch = batch_fixture;
    require(strings.set(batch, "before", "original"), "batch fixture setup");
    const auto original_before = batch_fixture->records["before"];
    batch_fixture->fail_batch = true;
    require(!strings.mset(batch, {{"before", "changed"}, {"new", "created"}}, false, applied) &&
            strings.status() == status::storage_error && !applied, "failed commit reported success");
    require(batch_fixture->records["before"] == original_before && !batch_fixture->records.count("new"), "failed commit mutated data");
    batch_fixture->fail_batch = false;
    require(strings.mset(batch, {{"before", "changed"}, {"new", "created"}}, false, applied) && applied, "batch commit failed");
    std::vector<pkv::dao::string_result> results;
    require(strings.mget(batch, {"new", "before", "new", "absent"}, results) && results.size() == 4 &&
            results[0].found && results[0].value == "created" && results[1].value == "changed" &&
            results[2].value == "created" && !results[3].found, "batch read ordering mismatch");
    batch_fixture->records["expired-ro"] = fixture_value(pkv::value_kind::string, "old", 0);
    require(strings.mget(batch, {"expired-ro", "before"}, results) && !results[0].found && results[1].found,
            "MGET returned an expired record");
    require(batch_fixture->records.count("expired-ro") == 1, "read-only snapshot unexpectedly committed expiry cleanup");
    require(hashes.hmset(batch, "hash-exists", {{"f", "v"}}) == 1, "EXISTS fixture");
    long long existing;
    require(keys.exists(batch, {"before", "before", "expired-ro", "hash-exists"}, existing) && existing == 3,
            "EXISTS duplicate or type count");
    require(strings.mset(batch, {{"before", "bad"}, {"other", "bad"}}, true, applied) && !applied &&
            !batch_fixture->records.count("other"), "MSETNX partly wrote a blocked request");
    batch_fixture->records["expired"] = fixture_value(pkv::value_kind::string, "old", 0);
    require(strings.mset(batch, {{"expired", "fresh"}, {"other", "fresh"}}, true, applied) && applied, "expired key blocked MSETNX");
    require(batch->transact({"before", "new", "before"}, [](pkv::db::batch_view entries) {
        require(entries.size() == 2, "batch did not deduplicate keys");
        for (auto& entry : entries) { entry.value = "not committed"; entry.action = pkv::db::update_action::write; }
        return false;
    }) == pkv::db::batch_status::aborted, "callback abort status mismatch");
    require(strings.get(batch, "before", out) && out == "changed", "callback abort committed a write");
    try {
        batch->transact({"new", "before"}, [](pkv::db::batch_view) -> bool { throw std::runtime_error("test batch exception"); });
        require(false, "batch exception disappeared");
    } catch (const std::runtime_error&) {}
    require(strings.mget(batch, {"before", "new"}, results), "batch exception retained locks");
    bool called = false;
    batch_fixture->fail_read = true;
    require(batch->transact({"before", "new"}, [&](pkv::db::batch_view) { called = true; return true; }) == pkv::db::batch_status::error &&
            !called, "failed batch lookup called mutation callback");
    batch_fixture->fail_read = false;
    require(strings.set(memory, "safe-delete", "safe"), "corrupt deletion fixture");
    require(memory->set("unsafe-delete", "not-json"), "corrupt deletion fixture");
    long long removed;
    require(!keys.del(memory, std::vector<std::string>{"safe-delete", "unsafe-delete"}, removed) &&
            removed == 0 && keys.status() == status::invalid_record, "corrupt DEL preflight status");
    require(strings.get(memory, "safe-delete", out) && out == "safe", "DEL partly deleted before detecting corruption");
    require(memory->set("transfer-a", "0") && memory->set("transfer-b", "0"), "transfer setup failed");
    threads.clear();
    ready = 0;
    for (int t = 0; t < 4; ++t) threads.emplace_back([&, t] {
        ++ready;
        while (ready.load() != 4) std::this_thread::yield();
        for (int f = 0; f < 8; ++f) go[&, t] {
            for (int i = 0; i < 20; ++i) {
                auto names = t % 2 ? std::vector<std::string>{"transfer-b", "transfer-a"}
                                   : std::vector<std::string>{"transfer-a", "transfer-b"};
                require(memory->transact(names, [](pkv::db::batch_view entries) {
                    auto* a = entries.find("transfer-a");
                    auto* b = entries.find("transfer-b");
                    require(a && b && a->found && b->found, "transfer lost a key");
                    // Raw fixture values are integers for this native batch test.
                    const auto av = std::stoll(a->value);
                    const auto bv = std::stoll(b->value);
                    require(av + bv == 0, "partial transfer snapshot");
                    acl::fiber::yield();
                    a->value = std::to_string(av + 1);
                    b->value = std::to_string(bv - 1);
                    a->action = b->action = pkv::db::update_action::write;
                    return true;
                }) == pkv::db::batch_status::ok, "yielding multi-key transfer failed");
            }
        };
        acl::fiber::schedule();
    });
    for (auto& thread : threads) thread.join();
    require(memory->get("transfer-a", out) && out == "640", "multi-key transfers lost increments");
    require(memory->get("transfer-b", out) && out == "-640", "multi-key transfers lost decrements");
    counting_watcher events;
    pkv::db_watchers watchers;
    watchers.add_watcher(&events);
    auto observed = pkv::db::create_mdb(4);
    require(observed->open("", &watchers) && observed->set("key", "local"), "observer fixture");
    require(events.sets == 1 && observed->replica_set("key", "replica") && events.sets == 1,
            "replica SET was re-emitted to observers");
    require(observed->get("key", out) && out == "replica", "replica SET did not apply");
    require(observed->replica_del("key") && observed->replica_del("key") && events.deletions == 0,
            "replica DEL was not idempotent or re-emitted an event");
    require(observed->set("key", "local-again") && observed->del("key") && events.sets == 2 && events.deletions == 1,
            "local observer events stopped working");
    for (bool erase : {false, true}) {
        bool entered = false;
        require(memory->set("replica-lock", "before") && memory->set("replica-aux", "before"), "replica isolation fixture");
        go[&] {
            require(memory->transact({"replica-lock", "replica-aux"}, [&](pkv::db::batch_view entries) {
                entered = true;
                acl::fiber::yield();
                for (auto& entry : entries) { entry.value = "local-after"; entry.action = pkv::db::update_action::write; }
                return true;
            }) == pkv::db::batch_status::ok, "replica isolation batch failed");
        };
        go[&, erase] {
            while (!entered) acl::fiber::yield();
            require(erase ? memory->replica_del("replica-lock") : memory->replica_set("replica-lock", "replica-after"),
                    "replica update failed");
        };
        acl::fiber::schedule();
        const bool found = memory->get("replica-lock", out);
        require(erase ? !found : found && out == "replica-after", "replication bypassed batch key locks");
        require(memory->get("replica-aux", out) && out == "local-after", "replica interfered with another batch key");
    }
    auto string_fixture = std::make_shared<fixture_db>();
    pkv::shared_db string_db = string_fixture;
    require(strings.set(string_db, "mutate", "7") && keys.expire(string_db, "mutate", 60), "string mutation fixture");
    const auto original_string = string_fixture->records.at("mutate");
    string_fixture->fail_write = true;
    long long number, length;
    require(!strings.increment(string_db, "mutate", 1, false, number) && strings.status() == status::storage_error,
            "INCR hid a failed write");
    require(!strings.append(string_db, "mutate", "x", length) && strings.status() == status::storage_error,
            "APPEND hid a failed write");
    require(!strings.setrange(string_db, "mutate", 0, "x", length) && strings.status() == status::storage_error,
            "SETRANGE hid a failed write");
    pkv::dao::get_options get_options;
    pkv::dao::string_result old_value;
    get_options.expiry = pkv::dao::get_options::persist;
    require(!strings.getex(string_db, "mutate", get_options, old_value) && strings.status() == status::storage_error,
            "GETEX hid a failed expiry write");
    get_options.expiry = pkv::dao::get_options::px;
    get_options.amount = "bad";
    require(!strings.getex(string_db, "mutate", get_options, old_value) && strings.status() == status::invalid_integer,
            "GETEX invalid integer reached storage write");
    string_fixture->fail_delete = true;
    get_options.expiry = pkv::dao::get_options::erase;
    require(!strings.getex(string_db, "mutate", get_options, old_value) && strings.status() == status::storage_error,
            "GETDEL hid a failed delete");
    require(string_fixture->records.at("mutate") == original_string, "failed string mutations changed data or TTL");
    string_fixture->fail_write = string_fixture->fail_delete = false;
    require(strings.getex(string_db, "mutate", get_options, old_value) && old_value.found && old_value.value == "7" &&
            !string_fixture->records.count("mutate"), "GETDEL did not consume the string");
    require(strings.set(string_db, "max-integer", "9223372036854775807"), "integer overflow fixture");
    const auto max_string = string_fixture->records.at("max-integer");
    require(!strings.increment(string_db, "max-integer", 1, false, number) && strings.status() == status::integer_overflow &&
            string_fixture->records.at("max-integer") == max_string, "integer overflow changed the record");
    auto hash_fixture = std::make_shared<fixture_db>();
    pkv::shared_db hash_db = hash_fixture;
    require(hashes.hset(hash_db, "hash-mutate", "counter", "7") && keys.expire(hash_db, "hash-mutate", 60), "hash mutation fixture");
    const auto original_hash = hash_fixture->records.at("hash-mutate");
    hash_fixture->fail_write = true;
    bool added, exists;
    require(!hashes.hincrby(hash_db, "hash-mutate", "counter", 1, number) && hashes.status() == status::storage_error,
            "HINCRBY hid failed write");
    require(!hashes.hsetnx(hash_db, "hash-mutate", "new", "v", added) && !added && hashes.status() == status::storage_error,
            "HSETNX hid failed write");
    require(hashes.hsetnx(hash_db, "hash-mutate", "counter", "bad", added) && !added, "existing HSETNX wrote unnecessarily");
    require(hash_fixture->records.at("hash-mutate") == original_hash, "failed hash writes changed bytes or TTL");
    hash_fixture->fail_read = true;
    require(!hashes.hincrby(hash_db, "hash-mutate", "counter", 1, number) && hashes.status() == status::storage_error,
            "HINCRBY hid failed read");
    require(!hashes.hsetnx(hash_db, "hash-mutate", "new", "v", added) && hashes.status() == status::storage_error,
            "HSETNX hid failed read");
    require(!hashes.hlen(hash_db, "hash-mutate", length) && hashes.status() == status::storage_error, "HLEN hid failed read");
    require(!hashes.hstrlen(hash_db, "hash-mutate", "counter", length, exists) && hashes.status() == status::storage_error,
            "HSTRLEN hid failed read");
    hash_fixture->fail_write = hash_fixture->fail_read = false;
    require(hashes.hlen(hash_db, "hash-mutate", length) && length == 1, "hash view size incorrect");
    require(hashes.hstrlen(hash_db, "hash-mutate", "counter", length, exists) && exists && length == 1, "hash field length incorrect");
    require(hashes.hincrby(hash_db, "hash-mutate", "counter", 2, number) && number == 9, "hash increment failed");
    require(hashes.hset(hash_db, "hash-mutate", "counter", "9223372036854775807"), "hash overflow fixture");
    const auto max_hash = hash_fixture->records.at("hash-mutate");
    require(!hashes.hincrby(hash_db, "hash-mutate", "counter", 1, number) && hashes.status() == status::integer_overflow &&
            hash_fixture->records.at("hash-mutate") == max_hash, "hash overflow changed record");
    auto list_fixture = std::make_shared<fixture_db>();
    pkv::shared_db list_db = list_fixture;
    pkv::dao::json_list lists;
    pkv::dao::list_result popped;
    std::vector<std::string> list_items;
    require(lists.push(list_db, "list", {"", binary, "last"}, false, false, length) && length == 3 &&
            keys.set_expiry(list_db, "list", 4102444800123LL, 0), "list fixture failed");
    const auto original_list = list_fixture->records.at("list");
    list_fixture->fail_write = true;
    require(!lists.push(list_db, "list", {"new"}, true, false, length) && lists.status() == status::storage_error,
            "LPUSH hid failed write");
    require(!lists.pop(list_db, "list", true, 1, popped) && lists.status() == status::storage_error,
            "LPOP hid failed write");
    require(!lists.set(list_db, "list", "1", "bad") && lists.status() == status::storage_error, "LSET hid failed write");
    require(!lists.trim(list_db, "list", 1, -1) && lists.status() == status::storage_error, "LTRIM hid failed write");
    require(lists.trim(list_db, "list", 0, -1), "no-op LTRIM unnecessarily wrote");
    list_fixture->fail_delete = true;
    require(!lists.pop(list_db, "list", false, 10, popped) && lists.status() == status::storage_error, "draining pop hid delete failure");
    require(!lists.trim(list_db, "list", 1, 0) && lists.status() == status::storage_error, "empty trim hid delete failure");
    require(list_fixture->records.at("list") == original_list, "failed list mutation changed bytes or TTL");
    list_fixture->fail_write = list_fixture->fail_delete = false;
    list_fixture->fail_read = true;
    require(!lists.length(list_db, "list", length) && lists.status() == status::storage_error, "LLEN hid read failure");
    require(!lists.range(list_db, "list", 0, -1, list_items) && lists.status() == status::storage_error, "LRANGE hid read failure");
    require(!lists.push(list_db, "list", {"new"}, false, false, length) && lists.status() == status::storage_error,
            "RPUSH hid read failure");
    list_fixture->fail_read = false;
    require(lists.set(list_db, "list", "-1", binary) && keys.expiry_time(list_db, "list", number) && number == 4102444800123LL,
            "LSET did not preserve list TTL");
    require(keys.persist(list_db, "list") && lists.range(list_db, "list", 0, -1, list_items) &&
            list_items == std::vector<std::string>{"", binary, binary}, "PERSIST changed list payload");
    require(lists.pop(list_db, "list", true, LLONG_MAX, popped) && popped.found && popped.values == list_items &&
            !list_fixture->records.count("list"), "list drain did not remove key");
    for (const auto& record : {std::string("{\"V\":1,\"T\":\"L\",\"E\":-1,\"P\":-1,\"D\":[5]}"),
                              std::string("{\"V\":9,\"T\":\"L\",\"E\":0,\"P\":0,\"D\":[\"x\"]}")}) {
        list_fixture->records["invalid-list"] = record;
        require(!lists.push(list_db, "invalid-list", {"x"}, false, false, length) &&
                !lists.pop(list_db, "invalid-list", true, 1, popped) &&
                !lists.length(list_db, "invalid-list", length) && list_fixture->records.at("invalid-list") == record,
                "list command overwrote corrupt/future data");
    }
    auto move_fixture = std::make_shared<batch_fixture_db>();
    pkv::shared_db move_db = move_fixture;
    auto move_codec = std::make_shared<rejecting_codec>();
    require(move_db->open("", nullptr, move_codec), "move codec open failed");
    require(lists.push(move_db, "source", {"", binary, "last"}, false, false, length) &&
            lists.push(move_db, "target", {"target-item"}, false, false, length) &&
            keys.set_expiry(move_db, "source", 4102444800123LL, 0) &&
            keys.set_expiry(move_db, "target", 4102444900456LL, 0), "atomic move fixture failed");
    const auto move_before = move_fixture->records;
    move_fixture->fail_batch = true;
    require(!lists.move(move_db, "source", "target", true, false, popped) && !popped.found && popped.values.empty() &&
            lists.status() == status::storage_error && move_fixture->records == move_before, "failed move commit lost value or TTL");
    move_fixture->fail_batch = false;
    move_codec->fail_at = move_codec->calls + 2; // destination encoded, source encoding fails
    require(!lists.move(move_db, "source", "target", true, false, popped) && !popped.found &&
            lists.status() == status::storage_error && move_fixture->records == move_before, "second move encode failure committed part of batch");
    move_codec->fail_at = -1;
    require(lists.move(move_db, "source", "target", true, false, popped) && popped.found && popped.values == std::vector<std::string>{""} &&
            keys.expiry_time(move_db, "source", number) && number == 4102444800123LL &&
            keys.expiry_time(move_db, "target", number) && number == 4102444900456LL, "successful move changed TTLs");
    require(strings.set(move_db, "wrong-target", "string"), "wrong destination fixture");
    const auto wrong_move = move_fixture->records;
    require(!lists.move(move_db, "source", "wrong-target", false, true, popped) && !popped.found &&
            lists.status() == status::wrong_type && move_fixture->records == wrong_move, "wrong target consumed source");
    move_fixture->records["future-target"] = "{\"V\":99}";
    require(lists.move(move_db, "missing-source", "future-target", false, true, popped) && !popped.found,
            "missing source decoded unrelated future destination");
    move_fixture->fail_read = true;
    require(!lists.move(move_db, "source", "target", false, true, popped) && lists.status() == status::storage_error,
            "move hid failed preflight read");
    move_fixture->fail_read = false;
    // Unsupported engines must never consume the source before discovering
    // that they cannot atomically write the destination.
    require(lists.push(list_db, "unsupported-source", {"x"}, false, false, length), "unsupported move fixture");
    const auto unsupported_before = list_fixture->records;
    require(!lists.move(list_db, "unsupported-source", "unsupported-target", false, true, popped) && !popped.found &&
            lists.status() == status::unsupported_operation && list_fixture->records == unsupported_before,
            "unsupported move partly changed values");
    list_fixture->fail_write = true;
    require(lists.move(list_db, "unsupported-source", "unsupported-source", false, false, popped) && popped.found &&
            popped.values == std::vector<std::string>{"x"}, "same-end self move unnecessarily wrote");
    list_fixture->fail_write = false;
    require(lists.push(list_db, "mpop-failure", {"a", binary, "c"}, false, false, length), "mpop fixture");
    list_fixture->records["mpop-expired"] = fixture_value(pkv::value_kind::list, "expired", 0);
    const auto mpop_before = list_fixture->records;
    std::string selected_key;
    list_fixture->fail_write = true;
    require(!lists.mpop(list_db, {"mpop-expired", "mpop-failure"}, true, 1, selected_key, popped) &&
            selected_key.empty() && !popped.found && lists.status() == status::storage_error &&
            list_fixture->records == mpop_before, "failed LMPOP changed values");
    list_fixture->fail_write = false;
    list_fixture->fail_delete = true;
    require(!lists.mpop(list_db, {"mpop-failure"}, false, 10, selected_key, popped) &&
            selected_key.empty() && !popped.found && lists.status() == status::storage_error &&
            list_fixture->records == mpop_before, "failed LMPOP deletion consumed items");
    list_fixture->fail_delete = false;
    require(lists.mpop(list_db, {"mpop-expired", "mpop-failure", "mpop-failure", "invalid-list"}, false, 2, selected_key, popped) &&
            selected_key == "mpop-failure" && popped.values == std::vector<std::string>{"c", binary} &&
            list_fixture->records.at("mpop-expired") == mpop_before.at("mpop-expired"),
            "LMPOP did not preserve order or defer expired-key cleanup");
    require(lists.range(list_db, "mpop-failure", 0, -1, list_items) && list_items == std::vector<std::string>{"a"},
            "LMPOP changed wrong range");
    require(lists.push(list_db, "list-edit", {"", binary, "", binary, "tail"}, false, false, length) &&
            keys.set_expiry(list_db, "list-edit", 4102444800123LL, 0), "list edit fixture failed");
    const auto edit_before = list_fixture->records.at("list-edit");
    list_fixture->fail_write = true;
    require(!lists.insert(list_db, "list-edit", true, binary, "new", length) && lists.status() == status::storage_error,
            "LINSERT hid a failed write");
    require(!lists.remove(list_db, "list-edit", -1, binary, number) && lists.status() == status::storage_error,
            "LREM hid a failed write");
    require(lists.insert(list_db, "list-edit", false, "missing", "new", length) && length == -1,
            "missing LINSERT pivot unnecessarily wrote");
    require(lists.remove(list_db, "list-edit", 0, "missing", number) && number == 0, "no-match LREM unnecessarily wrote");
    require(list_fixture->records.at("list-edit") == edit_before, "failed list edits changed bytes or TTL");
    list_fixture->fail_write = false;
    pkv::list_search search;
    search.rank = -1;
    search.count = 0;
    std::vector<long long> positions;
    list_fixture->fail_read = true;
    require(!lists.positions(list_db, "list-edit", binary, search, positions) && lists.status() == status::storage_error,
            "LPOS hid read failure");
    require(!lists.insert(list_db, "list-edit", true, binary, "new", length) && lists.status() == status::storage_error,
            "LINSERT hid read failure");
    require(!lists.remove(list_db, "list-edit", 0, binary, number) && lists.status() == status::storage_error,
            "LREM hid read failure");
    require(!lists.remove(list_db, "list-edit", LLONG_MIN, binary, number) && lists.status() == status::list_count_range,
            "LREM negated minimum signed integer");
    list_fixture->fail_read = false;
    require(lists.positions(list_db, "list-edit", binary, search, positions) && positions == std::vector<long long>{3, 1},
            "LPOS did not return reverse positions");
    require(list_fixture->records.at("list-edit") == edit_before, "LPOS rewrote record or TTL");
    require(lists.remove(list_db, "list-edit", -1, binary, number) && number == 1 &&
            lists.range(list_db, "list-edit", 0, -1, list_items) && list_items == std::vector<std::string>{"", binary, "", "tail"} &&
            keys.expiry_time(list_db, "list-edit", number) && number == 4102444800123LL, "LREM failed to preserve ordering or TTL");
    require(lists.insert(list_db, "list-edit", false, "", "inserted", length) && length == 5 &&
            keys.expiry_time(list_db, "list-edit", number) && number == 4102444800123LL, "LINSERT changed TTL");
    require(lists.push(list_db, "list-remove-all", {binary, binary}, false, false, length), "full removal fixture");
    const auto remove_before = list_fixture->records.at("list-remove-all");
    list_fixture->fail_delete = true;
    require(!lists.remove(list_db, "list-remove-all", 0, binary, number) && lists.status() == status::storage_error &&
            list_fixture->records.at("list-remove-all") == remove_before, "failed LREM deletion lost data");
    list_fixture->fail_delete = false;
    require(lists.remove(list_db, "list-remove-all", 0, binary, number) && number == 2 && !list_fixture->records.count("list-remove-all"),
            "LREM did not remove empty key");
    for (const auto& key : {"invalid-list", "list-search-corrupt"}) {
        list_fixture->records[key] = "{\"V\":1,\"T\":\"L\",\"E\":-1,\"P\":-1,\"D\":[5]}";
        const auto bad = list_fixture->records.at(key);
        require(!lists.insert(list_db, key, true, "5", "bad", length) && !lists.remove(list_db, key, 0, "5", number) &&
                !lists.positions(list_db, key, "5", search, positions) && list_fixture->records.at(key) == bad,
                "list edit/search changed corrupt records");
    }
    pkv::dao::json_set sets;
    auto set_fixture = std::make_shared<fixture_db>();
    pkv::shared_db set_db = set_fixture;
    require(sets.change(set_db, "set", {"", binary, "tail", binary}, true, number) && number == 3 &&
            keys.set_expiry(set_db, "set", 4102444800123LL, 0), "Set fixture failed");
    const auto original_set = set_fixture->records.at("set");
    set_fixture->fail_write = true;
    require(!sets.change(set_db, "set", {"new"}, true, number) && number == 0 && sets.status() == status::storage_error,
            "SADD hid failed write");
    require(!sets.change(set_db, "set", {binary}, false, number) && number == 0 && sets.status() == status::storage_error,
            "SREM hid failed write");
    require(sets.change(set_db, "set", {binary, ""}, true, number) && number == 0 &&
            sets.change(set_db, "set", {"missing"}, false, number) && number == 0, "Set no-op unnecessarily wrote");
    set_fixture->fail_delete = true;
    require(!sets.change(set_db, "set", {"", binary, "tail"}, false, number) && sets.status() == status::storage_error &&
            set_fixture->records.at("set") == original_set, "failed SREM deletion lost data or TTL");
    set_fixture->fail_write = set_fixture->fail_delete = false;
    set_fixture->fail_read = true;
    std::vector<long long> membership;
    std::set<std::string> set_members;
    require(!sets.size(set_db, "set", number) && sets.status() == status::storage_error &&
            !sets.membership(set_db, "set", {binary}, membership) && sets.status() == status::storage_error &&
            !sets.change(set_db, "set", {"new"}, true, number) && sets.status() == status::storage_error,
            "Set command hid read failure");
    set_fixture->fail_read = false;
    require(sets.membership(set_db, "set", {binary, "missing", "", binary}, membership) &&
            membership == std::vector<long long>{1, 0, 1, 1} && set_fixture->records.at("set") == original_set,
            "Set query changed data or TTL");
    for (const auto& bad : {std::string("{\"V\":1,\"T\":\"U\",\"E\":-1,\"P\":-1,\"D\":[\"a\",\"a\"]}"),
                            std::string("{\"V\":9,\"T\":\"U\",\"E\":0,\"P\":0,\"D\":[\"a\"]}")}) {
        set_fixture->records["bad-set"] = bad;
        require(!sets.change(set_db, "bad-set", {"x"}, true, number) && !sets.members(set_db, "bad-set", set_members) &&
                !sets.change(set_db, "bad-set", {"a"}, false, number) && set_fixture->records.at("bad-set") == bad,
                "Set command overwrote duplicate/future payload");
    }
    bool moved;
    require(!sets.move(set_db, "set", "new-target", binary, moved) && !moved &&
            sets.status() == status::unsupported_operation && set_fixture->records.at("set") == original_set &&
            !set_fixture->records.count("new-target"), "unsupported SMOVE partly committed");
    require(sets.move(set_db, "set", "set", binary, moved) && moved && set_fixture->records.at("set") == original_set,
            "self SMOVE changed bytes or TTL");
    require(sets.change(set_db, "existing-target", {binary}, true, number) &&
            keys.set_expiry(set_db, "existing-target", 4102444900456LL, 0), "existing SMOVE target fixture");
    const auto existing_target = set_fixture->records.at("existing-target");
    set_fixture->fail_write = true;
    require(!sets.move(set_db, "set", "existing-target", binary, moved) && !moved &&
            sets.status() == status::storage_error && set_fixture->records.at("set") == original_set &&
            set_fixture->records.at("existing-target") == existing_target, "failed single-change SMOVE lost data");
    set_fixture->fail_write = false;
    require(sets.move(set_db, "set", "existing-target", binary, moved) && moved &&
            set_fixture->records.at("existing-target") == existing_target &&
            sets.change(set_db, "set", {binary}, true, number) && number == 1 &&
            set_fixture->records.at("set") == original_set, "single-change SMOVE was rejected or changed target TTL");
    require(sets.change(set_db, "single-source", {binary}, true, number), "draining SMOVE fixture");
    const auto single_source = set_fixture->records.at("single-source");
    set_fixture->fail_delete = true;
    require(!sets.move(set_db, "single-source", "existing-target", binary, moved) && !moved &&
            sets.status() == status::storage_error && set_fixture->records.at("single-source") == single_source &&
            set_fixture->records.at("existing-target") == existing_target, "failed draining SMOVE consumed member");
    set_fixture->fail_delete = false;
    require(sets.move(set_db, "single-source", "existing-target", binary, moved) && moved &&
            !set_fixture->records.count("single-source"), "single-change SMOVE did not remove drained source");
    auto set_batch_fixture = std::make_shared<batch_fixture_db>();
    pkv::shared_db set_batch_db = set_batch_fixture;
    auto set_codec = std::make_shared<rejecting_codec>();
    require(set_batch_db->open("", nullptr, set_codec) &&
            sets.change(set_batch_db, "from", {binary, "left"}, true, number) &&
            sets.change(set_batch_db, "to", {"right"}, true, number) &&
            keys.set_expiry(set_batch_db, "from", 4102444800123LL, 0) &&
            keys.set_expiry(set_batch_db, "to", 4102444900456LL, 0), "SMOVE failure fixture");
    const auto before_smove = set_batch_fixture->records;
    set_batch_fixture->fail_batch = true;
    require(!sets.move(set_batch_db, "from", "to", binary, moved) && !moved && sets.status() == status::storage_error &&
            set_batch_fixture->records == before_smove, "failed SMOVE batch changed data/TTLs");
    set_batch_fixture->fail_batch = false;
    set_codec->fail_at = set_codec->calls + 2;
    require(!sets.move(set_batch_db, "from", "to", binary, moved) && !moved && sets.status() == status::storage_error &&
            set_batch_fixture->records == before_smove, "second SMOVE encoding failure partly committed");
    set_codec->fail_at = -1;
    require(sets.move(set_batch_db, "from", "to", binary, moved) && moved &&
            keys.expiry_time(set_batch_db, "from", number) && number == 4102444800123LL &&
            keys.expiry_time(set_batch_db, "to", number) && number == 4102444900456LL, "SMOVE changed key deadlines");
    const std::string store_target = "set-store";
    require(strings.set(set_db, store_target, "original") && keys.set_expiry(set_db, store_target, 4102444800123LL, 0),
            "Set store fixture");
    const auto before_store = set_fixture->records;
    set_fixture->fail_write = true;
    require(!sets.algebra(set_db, {"set"}, pkv::dao::set_operation::unite, &store_target, set_members) &&
            set_members.empty() && sets.status() == status::storage_error && set_fixture->records == before_store,
            "Set STORE hid failed write");
    set_fixture->fail_write = false;
    set_fixture->fail_delete = true;
    require(!sets.algebra(set_db, {"missing", "set"}, pkv::dao::set_operation::intersect, &store_target, set_members) &&
            sets.status() == status::storage_error && set_fixture->records == before_store, "empty Set STORE hid delete failure");
    set_fixture->fail_delete = false;
    require(sets.algebra(set_db, {"set", "set"}, pkv::dao::set_operation::unite, &store_target, set_members) &&
            set_members == std::set<std::string>{"", binary, "tail"} && keys.expiry_time(set_db, store_target, number) && number == -1,
            "Set STORE changed members or kept old TTL");
    set_fixture->records["expired-set"] = fixture_value(pkv::value_kind::set, "expired", 0);
    const auto snapshot_before = set_fixture->records;
    require(sets.algebra(set_db, {"expired-set", "set"}, pkv::dao::set_operation::unite, nullptr, set_members) &&
            set_members == std::set<std::string>{"", binary, "tail"} && set_fixture->records == snapshot_before,
            "read-only Set algebra cleaned expired records or rewrote data");
    require(!sets.algebra(set_db, {"missing", "bad-set"}, pkv::dao::set_operation::intersect, &store_target, set_members) &&
            set_fixture->records == snapshot_before, "empty first set skipped invalid later source");
    pkv::dao::set_sample_result sample;
    const auto sampling_before = set_fixture->records.at("set");
    set_fixture->fail_write = true;
    require(!sets.sample(set_db, "set", 1, true, sample) && !sample.found && sample.values.empty() &&
            sets.status() == status::storage_error && set_fixture->records.at("set") == sampling_before,
            "failed SPOP write consumed a sample");
    require(sets.sample(set_db, "set", 0, true, sample) && sample.found && sample.values.empty(), "SPOP zero unnecessarily wrote");
    set_fixture->fail_write = false;
    set_fixture->fail_delete = true;
    require(!sets.sample(set_db, "set", LLONG_MAX, true, sample) && !sample.found && sample.values.empty() &&
            sets.status() == status::storage_error && set_fixture->records.at("set") == sampling_before,
            "failed SPOP deletion lost members/TTL");
    set_fixture->fail_delete = false;
    set_fixture->fail_read = true;
    require(!sets.sample(set_db, "set", 1, false, sample) && sets.status() == status::storage_error &&
            !sets.sample(set_db, "set", 1, true, sample) && sets.status() == status::storage_error,
            "sampling hid failed lookup");
    set_fixture->fail_read = false;
    require(sets.sample(set_db, "set", -7, false, sample) && sample.found && sample.values.size() == 7 &&
            set_fixture->records.at("set") == sampling_before, "SRANDMEMBER changed bytes/TTL or count");
    const std::set<std::string> sampling_members = {"", binary, "tail"};
    for (const auto& member : sample.values) require(sampling_members.count(member) != 0, "SRANDMEMBER returned nonmember");
    require(sets.sample(set_db, "set", 1, true, sample) && sample.found && sample.values.size() == 1 &&
            keys.expiry_time(set_db, "set", number) && number == 4102444800123LL, "SPOP partial removal changed TTL");
    require(sets.members(set_db, "set", set_members) && set_members.size() == 2 && !set_members.count(sample.values.front()),
            "SPOP did not remove selected member");
    require(sets.sample(set_db, "set", LLONG_MAX, true, sample) && sample.values.size() == 2 && !set_fixture->records.count("set"),
            "SPOP did not delete drained key");
    require(sets.change(set_db, "card-small", {binary, "a"}, true, number) &&
            sets.change(set_db, "card-large", {binary, "a", "b", "c"}, true, number), "SINTERCARD fixture");
    const auto count_before = set_fixture->records;
    require(sets.intersection_size(set_db, {"card-small", "card-large", "card-small"}, 1, number) && number == 1 &&
            sets.intersection_size(set_db, {"card-small", "card-large"}, 0, number) && number == 2 &&
            set_fixture->records == count_before, "SINTERCARD mutated snapshot or ignored LIMIT");
    require(sets.intersection_size(set_db, {"expired-set", "card-small"}, 1, number) && number == 0 &&
            set_fixture->records == count_before, "SINTERCARD physically cleaned expired key");
    require(!sets.intersection_size(set_db, {"missing", "bad-set"}, 1, number) && number == 0 &&
            set_fixture->records == count_before, "SINTERCARD skipped invalid source after missing key");
    set_fixture->fail_read = true;
    require(!sets.intersection_size(set_db, {"card-small", "card-large"}, 1, number) && number == 0 &&
            sets.status() == status::storage_error, "SINTERCARD hid failed preflight");
    set_fixture->fail_read = false;
    pkv::dao::json_scan scanner;
    pkv::scan_options scanning;
    pkv::scan_page page;
    require(!scanner.keys(set_db, scanning, page) && scanner.status() == status::unsupported_scan,
            "unsupported backend scan did not report explicit error");
    auto scan_fixture = std::make_shared<scan_fixture_db>();
    pkv::shared_db scan_db = scan_fixture;
    require(strings.set(scan_db, "scan:live", binary) && strings.set(scan_db, "scan:expired", "old") &&
            keys.set_expiry(scan_db, "scan:expired", 1, 0), "scan filter fixture");
    scan_fixture->records["ignore:corrupt"] = "bad bytes";
    scan_fixture->records["scan:expired"] = fixture_value(pkv::value_kind::string, "old", 0);
    scanning.has_match = true;
    scanning.pattern = "scan:*";
    require(scanner.keys(scan_db, scanning, page) && page.cursor == 0 && page.values == std::vector<std::string>{"scan:live"} &&
            !scan_fixture->records.count("scan:expired"), "SCAN returned expired data or decoded unrelated corruption");
    scan_fixture->fail_scan = true;
    require(!scanner.keys(scan_db, scanning, page) && scanner.status() == status::storage_error && page.values.empty(), "SCAN hid enumeration failure");
    scan_fixture->fail_scan = false;
    scan_fixture->fail_read = true;
    require(!scanner.keys(scan_db, scanning, page) && scanner.status() == status::storage_error, "SCAN hid logical lookup failure");
    scan_fixture->fail_read = false;
    scanning.has_type = true;
    scanning.type = "set";
    require(scanner.keys(scan_db, scanning, page) && page.values.empty(), "SCAN TYPE ignored value kind");
    require(sets.change(scan_db, "collection", {"", binary, "stable"}, true, number), "SSCAN fixture");
    const auto collection_bytes = scan_fixture->records.at("collection");
    scanning = pkv::scan_options();
    scanning.count = 1;
    std::set<std::string> scanned;
    do {
        require(scanner.open_collection(scan_db, "collection", pkv::value_kind::set) && scanner.collection(scanning, page), "SSCAN owning view failed");
        scanned.insert(page.values.begin(), page.values.end());
        scanning.cursor = page.cursor;
    } while (scanning.cursor);
    require(scanned == std::set<std::string>{"", binary, "stable"} && scan_fixture->records.at("collection") == collection_bytes,
            "SSCAN omitted members or rewrote bytes");
    require(!scanner.open_collection(scan_db, "collection", pkv::value_kind::hash) && scanner.status() == status::wrong_type, "HSCAN accepted a Set");
    scan_fixture->records["bad-hscan"] = "{\"T\":\"H\",\"E\":-1,\"D\":{\"a\":\"1\",\"a\":\"2\"}}";
    const auto bad_scan = scan_fixture->records.at("bad-hscan");
    require(!scanner.open_collection(scan_db, "bad-hscan", pkv::value_kind::hash) &&
            scanner.status() == status::invalid_record && scan_fixture->records.at("bad-hscan") == bad_scan, "HSCAN hid duplicate-field corruption");
    auto enumerated = pkv::db::create_mdb(16);
    require(enumerated->open(""), "native scan DB open failed");
    std::set<std::string> stable_keys;
    for (int i = 0; i < 200; ++i) {
        const auto key = "stable-scan:" + std::to_string(i);
        stable_keys.insert(key);
        require(enumerated->set(key, "raw"), "native scan fixture failed");
    }
    std::thread churn([&] {
        for (int i = 0; i < 1000; ++i) {
            require(enumerated->set("transient:" + std::to_string(i), "raw"), "scan churn write failed");
            if (i) enumerated->del("transient:" + std::to_string(i - 1));
        }
    });
    unsigned long long native_cursor = 0, next_cursor;
    std::vector<std::string> raw_keys;
    scanned.clear();
    do {
        require(enumerated->scan_keys(native_cursor, 2, raw_keys, next_cursor) == pkv::db::scan_status::ok, "native key scan failed");
        scanned.insert(raw_keys.begin(), raw_keys.end());
        native_cursor = next_cursor;
    } while (native_cursor);
    churn.join();
    for (const auto& key : stable_keys) require(scanned.count(key) != 0, "scan lost continuously present key during rehash");
    require(enumerated->scan_keys(ULLONG_MAX, 1, raw_keys, next_cursor) == pkv::db::scan_status::ok && raw_keys.empty() && next_cursor == 0,
            "native scan cursor wrapped/truncated");
    // One busy logical shard must no longer become one oversized scan page.
    auto paged = pkv::db::create_mdb(1);
    require(paged->open(""), "paged scan fixture open failed");
    std::set<std::string> paged_keys;
    for (int i = 0; i < 4096; ++i) {
        const auto key = "bounded-scan:" + std::to_string(i) + std::string("\0key", 4);
        paged_keys.insert(key);
        require(paged->set(key, "raw"), "paged scan fixture write failed");
    }
    native_cursor = 0;
    scanned.clear();
    size_t pages = 0, max_page = 0, total = 0;
    do {
        require(paged->scan_keys(native_cursor, 1, raw_keys, next_cursor) == pkv::db::scan_status::ok, "paged scan failed");
        require(next_cursor == 0 || next_cursor > native_cursor, "native cursor failed to advance");
        ++pages;
        total += raw_keys.size();
        max_page = std::max(max_page, raw_keys.size());
        scanned.insert(raw_keys.begin(), raw_keys.end());
        native_cursor = next_cursor;
    } while (native_cursor);
    require(scanned == paged_keys && total == paged_keys.size() && pages > 1 && max_page < 256,
            "COUNT 1 still copied the entire busy shard or lost/duplicated keys");
    require(paged->scan_keys(0, 4096, raw_keys, next_cursor) == pkv::db::scan_status::ok && raw_keys.size() == 4096,
            "large scan hint changed enumeration");
    for (const auto& key : paged_keys) require(paged->del(key), "bucket deletion failed");
    require(paged->scan_keys(0, 1, raw_keys, next_cursor) == pkv::db::scan_status::ok && raw_keys.empty() && next_cursor == 0,
            "empty allocated buckets appeared as keys");
    require(paged->set("reuse", "first") && paged->set("reuse", "second") && paged->get("reuse", out) && out == "second",
            "bucket reuse or overwrite failed");
    pkv::dao::json_zset sorted;
    pkv::dao::zadd_options zoptions;
    pkv::dao::zadd_result zout;
    auto zfixture = std::make_shared<fixture_db>();
    pkv::shared_db zdb = zfixture;
    require(sorted.add(zdb, "sorted", {{"", 1}, {binary, 2}}, zoptions, zout) && zout.added == 2 &&
            keys.set_expiry(zdb, "sorted", 4102444800123LL, 0), "Sorted Set mutation fixture failed");
    const auto zbytes = zfixture->records.at("sorted");
    zfixture->fail_write = true;
    require(!sorted.add(zdb, "sorted", {{"new", 3}}, zoptions, zout) && zout.added == 0 && sorted.status() == status::storage_error,
            "ZADD hid write failure");
    require(!sorted.remove(zdb, "sorted", {binary}, number) && number == 0 && sorted.status() == status::storage_error, "ZREM hid write failure");
    std::vector<pkv::zset_member> zvalues;
    require(!sorted.pop(zdb, "sorted", 1, false, zvalues) && zvalues.empty() && sorted.status() == status::storage_error,
            "ZPOP hid failed write");
    zoptions.nx = true;
    require(sorted.add(zdb, "sorted", {{binary, 9}}, zoptions, zout) && !zout.processed, "no-op ZADD unnecessarily wrote");
    zoptions.nx = false;
    require(sorted.add(zdb, "sorted", {{binary, 2}}, zoptions, zout) && zout.changed == 0 &&
            zfixture->records.at("sorted") == zbytes, "unchanged ZADD rewrote bytes or TTL");
    zfixture->fail_write = false;
    zfixture->fail_delete = true;
    require(!sorted.pop(zdb, "sorted", LLONG_MAX, true, zvalues) && zvalues.empty() &&
            sorted.status() == status::storage_error && zfixture->records.at("sorted") == zbytes, "failed ZPOP deletion consumed members");
    zfixture->fail_delete = false;
    zfixture->fail_read = true;
    require(!sorted.add(zdb, "sorted", {{"new", 3}}, zoptions, zout) && sorted.status() == status::storage_error &&
            !sorted.range(zdb, "sorted", 0, -1, false, zvalues) && sorted.status() == status::storage_error,
            "Sorted Set command hid lookup failure");
    zfixture->fail_read = false;
    require(sorted.add(zdb, "sorted", {{binary, 3}}, zoptions, zout) && zout.changed == 1 &&
            keys.expiry_time(zdb, "sorted", number) && number == 4102444800123LL &&
            keys.persist(zdb, "sorted") && sorted.range(zdb, "sorted", 0, -1, false, zvalues) &&
            zvalues.size() == 2 && zvalues[1].member == binary && zvalues[1].score == 3, "Sorted Set TTL mutation changed payload");
    const auto infinite = std::numeric_limits<double>::infinity();
    require(sorted.add(zdb, "infinite", {{binary, infinite}}, zoptions, zout), "infinity fixture failed");
    const auto infinity_before = zfixture->records.at("infinite");
    zoptions.incr = true;
    require(!sorted.add(zdb, "infinite", {{binary, -infinite}}, zoptions, zout) && sorted.status() == status::nan_score &&
            zfixture->records.at("infinite") == infinity_before, "NaN arithmetic overwrote score");
    zoptions.incr = false;
    for (const auto& raw : {std::string("{\"V\":1,\"T\":\"Z\",\"E\":-1,\"P\":-1,\"D\":[[\"a\",\"1\"],[\"a\",\"2\"]]}"),
                            std::string("{\"V\":99,\"T\":\"Z\",\"E\":0,\"P\":0,\"D\":[[\"a\",\"1\"]]}")}) {
        zfixture->records["bad-zset"] = raw;
        require(!sorted.add(zdb, "bad-zset", {{"new", 2}}, zoptions, zout) &&
                !sorted.pop(zdb, "bad-zset", 1, false, zvalues) && zfixture->records.at("bad-zset") == raw,
                "Sorted Set mutation overwrote corrupt/future bytes");
    }
    pkv::scan_options zscan_options;
    zscan_options.count = 1;
    std::map<std::string, std::string> zscanned;
    do {
        require(scanner.open_collection(zdb, "sorted", pkv::value_kind::zset) && scanner.collection(zscan_options, page), "ZSCAN storage fixture failed");
        require(page.values.size() % 2 == 0, "ZSCAN split member/score pair");
        for (size_t i = 0; i < page.values.size(); i += 2) zscanned.emplace(page.values[i], page.values[i + 1]);
        zscan_options.cursor = page.cursor;
    } while (zscan_options.cursor);
    require(zscanned == std::map<std::string, std::string>{{"", "1"}, {binary, "3"}}, "ZSCAN storage lost scores");
    const auto zscan_before = zfixture->records.at("sorted");
    zfixture->fail_write = zfixture->fail_delete = true;
    require(scanner.open_collection(zdb, "sorted", pkv::value_kind::zset) && scanner.collection(zscan_options, page) &&
            zfixture->records.at("sorted") == zscan_before, "ZSCAN attempted live-record writes");
    zfixture->fail_write = zfixture->fail_delete = false;
    zfixture->fail_read = true;
    require(!scanner.open_collection(zdb, "sorted", pkv::value_kind::zset) && scanner.status() == status::storage_error &&
            !scanner.collection(zscan_options, page) && page.values.empty() && page.cursor == 0, "ZSCAN reused a view after failed read");
    zfixture->fail_read = false;
    require(!scanner.open_collection(zdb, "sorted", pkv::value_kind::set) && scanner.status() == status::wrong_type &&
            !scanner.collection(zscan_options, page) && page.values.empty(), "collection scan reused wrong-type view");
    for (const auto& raw : {std::string("bad bytes"), std::string("{\"V\":99,\"T\":\"Z\",\"E\":-1,\"D\":[]}"),
                            std::string("{\"V\":1,\"T\":\"Z\",\"E\":-1,\"P\":-1,\"D\":[[\"a\",\"1\"],[\"a\",\"2\"]]}")}) {
        zfixture->records["bad-zscan"] = raw;
        require(!scanner.open_collection(zdb, "bad-zscan", pkv::value_kind::zset) &&
                zfixture->records.at("bad-zscan") == raw && !scanner.collection(zscan_options, page) && page.values.empty(),
                "ZSCAN hid invalid/future record or rewrote bytes");
    }
    pkv::dao::zset_sample_result zsample;
    zfixture->fail_write = zfixture->fail_delete = true;
    require(sorted.sample(zdb, "sorted", 99, zsample) && zsample.found && zsample.values.size() == 2 &&
            zfixture->records.at("sorted") == zscan_before, "ZRANDMEMBER tried to rewrite live data");
    require(sorted.sample(zdb, "absent-zrandom", -3, zsample) && !zsample.found && zsample.values.empty(), "missing sample reused previous output");
    zfixture->fail_write = zfixture->fail_delete = false;
    zfixture->fail_read = true;
    require(!sorted.sample(zdb, "sorted", 1, zsample) && !zsample.found && zsample.values.empty() && sorted.status() == status::storage_error,
            "Sorted Set sample hid lookup failure");
    zfixture->fail_read = false;
    require(!sorted.sample(zdb, "bad-zset", 1, zsample) && zsample.values.empty() &&
            !sorted.sample(zdb, "sorted", LLONG_MIN, zsample) && sorted.status() == status::invalid_integer,
            "sample accepted a future record or negated LLONG_MIN");
    require(!sorted.sample(zdb, "sorted", -LLONG_MAX, zsample) && !zsample.found && zsample.values.empty() &&
            sorted.status() == status::sample_too_large && zfixture->records.at("sorted") == zscan_before,
            "oversized sample changed stored record or tried to allocate");
    pkv::zset_query query;
    query.mode = pkv::zrange_mode::score;
    query.min_score.value = 1; query.max_score.value = 3;
    require(sorted.store_range(zdb, "sorted", "range-dest", query, number) && number == 2,
            "single-write range store rejected a legacy backend");
    const auto destination_before = zfixture->records.at("range-dest");
    const auto source_before = zfixture->records.at("sorted");
    zfixture->fail_write = true;
    require(!sorted.store_range(zdb, "sorted", "range-dest", query, number) && number == 0 && sorted.status() == status::storage_error &&
            zfixture->records.at("range-dest") == destination_before && zfixture->records.at("sorted") == source_before,
            "range store hid write failure or changed source");
    query.max_score.value = 1;
    require(!sorted.remove_range(zdb, "sorted", query, number) && number == 0 && sorted.status() == status::storage_error &&
            zfixture->records.at("sorted") == source_before, "range removal hid failed rewrite");
    query.min_score.value = 50;
    require(sorted.remove_range(zdb, "sorted", query, number) && number == 0 && zfixture->records.at("sorted") == source_before,
            "empty range removal rewrote bytes");
    zfixture->fail_write = false; zfixture->fail_delete = true;
    require(!sorted.store_range(zdb, "sorted", "range-dest", query, number) && number == 0 &&
            zfixture->records.at("range-dest") == destination_before, "empty range store hid deletion failure");
    query.min_score.value = 1; query.max_score.value = 3;
    require(!sorted.remove_range(zdb, "sorted", query, number) && number == 0 && zfixture->records.at("sorted") == source_before,
            "range removal consumed members on failed deletion");
    zfixture->fail_delete = false; zfixture->fail_read = true;
    require(!sorted.store_range(zdb, "sorted", "range-dest", query, number) && number == 0 && sorted.status() == status::storage_error &&
            !sorted.select(zdb, "sorted", query, zvalues) && zvalues.empty() && !sorted.count(zdb, "sorted", query, number) && number == 0,
            "range read/store hid lookup failure");
    zfixture->fail_read = false;
    require(!sorted.store_range(zdb, "bad-zset", "range-dest", query, number) && number == 0 &&
            zfixture->records.at("range-dest") == destination_before, "future source overwrote range destination");
    auto range_codec = std::make_shared<rejecting_codec>();
    require(zdb->open("", nullptr, range_codec), "range codec open failed");
    range_codec->fail_at = range_codec->calls + 1;
    require(!sorted.store_range(zdb, "sorted", "range-dest", query, number) && number == 0 &&
            zfixture->records.at("range-dest") == destination_before && zfixture->records.at("sorted") == source_before,
            "range store published failed encoding");
    auto mpop_fixture = std::make_shared<fixture_db>();
    pkv::shared_db mpop_db = mpop_fixture;
    mpop_fixture->records["a"] = source_before;
    mpop_fixture->records["b"] = source_before;
    require(keys.set_expiry(mpop_db, "b", 4102444800123LL, 0), "ZMPOP TTL fixture failed");
    const auto zmpop_before = mpop_fixture->records;
    pkv::dao::zset_pop_result mpopped;
    std::string zselected_key;
    mpop_fixture->fail_write = true;
    require(!sorted.mpop(mpop_db, {"absent", "b", "a"}, false, 1, zselected_key, mpopped) && !mpopped.found &&
            mpopped.values.empty() && zselected_key.empty() && sorted.status() == status::storage_error && mpop_fixture->records == zmpop_before,
            "failed ZMPOP rewrite lost members or returned uncommitted values");
    mpop_fixture->fail_write = false; mpop_fixture->fail_delete = true;
    require(!sorted.mpop(mpop_db, {"b", "a"}, true, LLONG_MAX, zselected_key, mpopped) && !mpopped.found &&
            mpopped.values.empty() && zselected_key.empty() && sorted.status() == status::storage_error && mpop_fixture->records == zmpop_before,
            "failed ZMPOP deletion returned consumed members");
    mpop_fixture->fail_delete = false; mpop_fixture->fail_read = true;
    require(!sorted.mpop(mpop_db, {"b", "a"}, true, 1, zselected_key, mpopped) && zselected_key.empty() &&
            mpopped.values.empty() && sorted.status() == status::storage_error, "ZMPOP hid transaction lookup error");
    mpop_fixture->fail_read = false;
    auto mpop_codec = std::make_shared<rejecting_codec>();
    require(mpop_db->open("", nullptr, mpop_codec), "ZMPOP codec fixture open failed");
    mpop_codec->fail_at = mpop_codec->calls + 1;
    require(!sorted.mpop(mpop_db, {"b", "a"}, false, 1, zselected_key, mpopped) && !mpopped.found &&
            mpopped.values.empty() && zselected_key.empty() && mpop_fixture->records == zmpop_before, "ZMPOP published failed encoding");
    mpop_codec->fail_at = -1;
    mpop_fixture->records["bad"] = "future or corrupt record";
    const auto untouched_a = mpop_fixture->records.at("a");
    require(!sorted.mpop(mpop_db, {"bad", "b"}, true, 1, zselected_key, mpopped) && !mpopped.found && zselected_key.empty() &&
            mpop_fixture->records.at("b") == zmpop_before.at("b"), "ZMPOP skipped corrupt first key");
    require(sorted.mpop(mpop_db, {"b", "b", "bad", "a"}, true, 1, zselected_key, mpopped) && mpopped.found && zselected_key == "b" &&
            mpopped.values.size() == 1 && mpopped.values[0].member == binary && mpopped.values[0].score == 3 &&
            mpop_fixture->records.at("a") == untouched_a && keys.expiry_time(mpop_db, "b", number) && number == 4102444800123LL,
            "ZMPOP ignored request order, repeated a pop, decoded a later key or changed TTL");
    require(sorted.mpop(mpop_db, {"b", "a"}, false, LLONG_MAX, zselected_key, mpopped) && zselected_key == "b" &&
            mpopped.values.size() == 1 && mpopped.values[0].member.empty() && !mpop_fixture->records.count("b") &&
            mpop_fixture->records.at("a") == untouched_a, "ZMPOP did not delete exactly the drained key");
    require(sorted.mpop(mpop_db, {"b", "absent"}, false, 1, zselected_key, mpopped) && !mpopped.found && zselected_key.empty() &&
            mpopped.values.empty(), "missing ZMPOP retained previous output");
    require(!sorted.mpop(mpop_db, {}, false, 1, zselected_key, mpopped) &&
            !sorted.mpop(mpop_db, {"a"}, false, 0, zselected_key, mpopped) && sorted.status() == status::invalid_integer,
            "ZMPOP accepted invalid direct DAO parameters");
    auto algebra_fixture = std::make_shared<fixture_db>();
    pkv::shared_db algebra_db = algebra_fixture;
    algebra_fixture->records["source"] = source_before;
    require(sets.change(algebra_db, "set-source", {"", binary, "set-only"}, true, number) &&
            strings.set(algebra_db, "target", "old"), "Sorted Set algebra fixture failed");
    const auto algebra_before = algebra_fixture->records;
    auto default_algebra = [](pkv::dao::zset_algebra_options&) { return true; };
    std::vector<pkv::zset_member> algebra_out;
    long long algebra_count;
    const std::string algebra_destination = "target";
    algebra_fixture->fail_write = algebra_fixture->fail_delete = true;
    require(sorted.algebra(algebra_db, {"source", "set-source", "source"}, pkv::dao::zset_operation::intersect, nullptr, false,
            default_algebra, algebra_out, algebra_count) && algebra_count == 2 && algebra_out.size() == 2 &&
            algebra_out[0].member.empty() && algebra_out[0].score == 3 && algebra_out[1].member == binary && algebra_out[1].score == 7 &&
            algebra_fixture->records == algebra_before, "read-only weighted intersection changed bytes or duplicate score contributions");
    require(!sorted.algebra(algebra_db, {"source"}, pkv::dao::zset_operation::unite, &algebra_destination, false,
            default_algebra, algebra_out, algebra_count) && algebra_count == 0 && algebra_out.empty() &&
            sorted.status() == status::storage_error && algebra_fixture->records == algebra_before, "algebra store hid failed write");
    algebra_fixture->fail_write = false;
    require(!sorted.algebra(algebra_db, {"source", "source"}, pkv::dao::zset_operation::difference, &algebra_destination, false,
            default_algebra, algebra_out, algebra_count) && algebra_count == 0 && algebra_fixture->records == algebra_before,
            "empty algebra store hid failed deletion");
    algebra_fixture->fail_delete = false; algebra_fixture->fail_read = true;
    require(!sorted.algebra(algebra_db, {"source"}, pkv::dao::zset_operation::unite, nullptr, false, default_algebra,
            algebra_out, algebra_count) && sorted.status() == status::storage_error && algebra_count == 0, "algebra hid source lookup failure");
    algebra_fixture->fail_read = false;
    bool parsed_options = false;
    auto observed_parser = [&](pkv::dao::zset_algebra_options&) { parsed_options = true; return true; };
    require(!sorted.algebra(algebra_db, {"absent", "target"}, pkv::dao::zset_operation::intersect, nullptr, true, observed_parser,
            algebra_out, algebra_count) && !parsed_options && sorted.status() == status::wrong_type,
            "algebra parsed options or returned early before validating wrong-type source");
    algebra_fixture->records["corrupt"] = "invalid stored bytes";
    require(!sorted.algebra(algebra_db, {"absent", "corrupt"}, pkv::dao::zset_operation::intersect, nullptr, true, default_algebra,
            algebra_out, algebra_count) && sorted.status() == status::invalid_record && algebra_count == 0, "ZINTERCARD ignored corrupt later source");
    auto algebra_codec = std::make_shared<rejecting_codec>();
    require(algebra_db->open("", nullptr, algebra_codec), "algebra codec fixture failed");
    algebra_codec->fail_at = algebra_codec->calls + 1;
    require(!sorted.algebra(algebra_db, {"source"}, pkv::dao::zset_operation::unite, &algebra_destination, false, default_algebra,
            algebra_out, algebra_count) && algebra_count == 0 && algebra_out.empty() && algebra_fixture->records.at("target") == algebra_before.at("target"),
            "algebra store published failed encoding");
    algebra_codec->fail_at = -1;
    require(!sorted.algebra(algebra_db, {"source"}, pkv::dao::zset_operation::unite, &algebra_destination, false,
            [](pkv::dao::zset_algebra_options&) { return false; }, algebra_out, algebra_count) && algebra_count == 0 &&
            algebra_fixture->records.at("target") == algebra_before.at("target"), "aborted option parser published store");
    require(sorted.algebra(algebra_db, {"source", "set-source"}, pkv::dao::zset_operation::unite, &algebra_destination, false,
            default_algebra, algebra_out, algebra_count) && algebra_count == 3 && algebra_out.empty() &&
            keys.ttl(algebra_db, "target", number) && number == -1 && algebra_fixture->records.at("source") == source_before,
            "single-write legacy algebra store failed or changed source/TTL");
    require(!sorted.algebra(algebra_db, {"source"}, pkv::dao::zset_operation::unite, nullptr, false,
            [](pkv::dao::zset_algebra_options& options) { options.weights[0] = std::numeric_limits<double>::quiet_NaN(); return true; },
            algebra_out, algebra_count) && algebra_count == 0 && sorted.status() == status::invalid_score,
            "direct algebra API accepted NaN weight");
    auto rename_fixture = std::make_shared<batch_fixture_db>();
    pkv::shared_db rename_db = rename_fixture;
    auto rename_codec = std::make_shared<rejecting_codec>();
    require(rename_db->open("", nullptr, rename_codec), "rename fixture open failed");
    rename_fixture->records["source"] = source_before;
    require(strings.set(rename_db, "destination", "old"), "rename destination fixture failed");
    const auto rename_before = rename_fixture->records;
    bool renamed;
    rename_fixture->fail_batch = true;
    require(!keys.rename(rename_db, "source", "destination", false, renamed) && !renamed && keys.status() == status::storage_error &&
            rename_fixture->records == rename_before, "failed rename batch partially changed source/destination");
    rename_fixture->fail_batch = false; rename_fixture->fail_read = true;
    require(!keys.rename(rename_db, "source", "destination", false, renamed) && !renamed && keys.status() == status::storage_error,
            "rename hid lookup failure");
    rename_fixture->fail_read = false;
    const auto encode_calls = rename_codec->calls;
    rename_codec->fail_at = encode_calls + 1;
    require(keys.rename(rename_db, "source", "destination", true, renamed) && !renamed && rename_fixture->records == rename_before,
            "RENAMENX overwrote live destination");
    require(keys.rename(rename_db, "source", "destination", false, renamed) && renamed && !rename_fixture->records.count("source") &&
            rename_fixture->records.at("destination") == source_before && rename_codec->calls == encode_calls,
            "rename rewrote codec bytes or needed an encoder");
    rename_fixture->records["legacy-source"] = fixture_value(pkv::value_kind::string, "legacy", 4102444800000);
    const auto legacy_rename = rename_fixture->records.at("legacy-source");
    require(keys.rename(rename_db, "legacy-source", "legacy-destination", false, renamed) && renamed &&
            rename_fixture->records.at("legacy-destination") == legacy_rename && keys.expiry_time(rename_db, "legacy-destination", number) &&
            number == 4102444800000LL, "rename migrated legacy bytes or changed expiry");
    for (const auto& bad : {std::string("broken"), std::string("{\"V\":99,\"T\":\"S\",\"E\":0,\"P\":0,\"D\":\"old\"}"),
                            std::string("{\"T\":\"H\",\"E\":-1,\"D\":{\"a\":\"1\",\"a\":\"2\"}}"),
                            std::string("{\"T\":\"H\",\"E\":-1,\"D\":{\"a\":1}}")}) {
        rename_fixture->records["bad-source"] = bad;
        require(!keys.rename(rename_db, "bad-source", "bad-source", false, renamed) && !renamed &&
                !keys.rename(rename_db, "bad-source", "destination", false, renamed) && !renamed &&
                rename_fixture->records.at("bad-source") == bad && rename_fixture->records.at("destination") == source_before,
                "rename moved or discarded corrupt/future source bytes");
    }
    auto unsupported_rename = std::make_shared<fixture_db>();
    pkv::shared_db unsupported_rename_db = unsupported_rename;
    unsupported_rename->records["source"] = source_before;
    require(!keys.rename(unsupported_rename_db, "source", "destination", false, renamed) && !renamed &&
            keys.status() == status::unsupported_operation && unsupported_rename->records.size() == 1 &&
            unsupported_rename->records.at("source") == source_before, "legacy rename used a partial-write fallback");
    require(keys.rename(unsupported_rename_db, "source", "source", false, renamed) && renamed &&
            keys.rename(unsupported_rename_db, "source", "source", true, renamed) && !renamed,
            "same-key rename unnecessarily required multi-write backend support");
    auto copy_from_fixture = std::make_shared<fixture_db>();
    auto copy_to_fixture = std::make_shared<fixture_db>();
    pkv::shared_db copy_from = copy_from_fixture, copy_to = copy_to_fixture;
    copy_from_fixture->records["same"] = source_before;
    require(strings.set(copy_to, "same", "old"), "COPY fixture failed");
    const auto copy_target_before = copy_to_fixture->records.at("same");
    bool copied;
    copy_to_fixture->fail_write = true;
    require(!keys.copy(copy_from, "same", copy_to, "same", true, copied) && !copied && keys.status() == status::storage_error &&
            copy_to_fixture->records.at("same") == copy_target_before && copy_from_fixture->records.at("same") == source_before,
            "failed cross-DB COPY changed source/target");
    copy_to_fixture->fail_write = false;
    copy_from_fixture->fail_read = true;
    require(!keys.copy(copy_from, "same", copy_to, "same", true, copied) && !copied && keys.status() == status::storage_error,
            "COPY hid source lookup failure");
    copy_from_fixture->fail_read = false;
    copy_to_fixture->fail_read = true;
    require(!keys.copy(copy_from, "same", copy_to, "same", true, copied) && !copied && keys.status() == status::storage_error,
            "COPY hid target lookup failure");
    copy_to_fixture->fail_read = false;
    require(keys.copy(copy_from, "same", copy_to, "same", false, copied) && !copied &&
            keys.copy(copy_from, "same", copy_to, "same", true, copied) && copied &&
            copy_to_fixture->records.at("same") == source_before && copy_from_fixture->records.at("same") == source_before,
            "COPY failed same-name cross-DB replacement or altered source");
    require(keys.copy(copy_from, "missing", copy_to, "same", true, copied) && !copied &&
            !keys.copy(copy_from, "same", copy_from, "same", true, copied) && !copied && keys.status() == status::same_object,
            "COPY retained result or accepted identical source object");
    auto copy_codec = std::make_shared<rejecting_codec>();
    require(copy_to->open("", nullptr, copy_codec), "COPY rejecting encoder fixture failed");
    copy_codec->fail_at = 1;
    require(!keys.copy(copy_from, "same", copy_to, "same", true, copied) && !copied &&
            copy_to_fixture->records.at("same") == source_before && copy_from_fixture->records.at("same") == source_before,
            "failed cross-codec COPY published partial encoding");
    std::atomic<int> pool_creations{0};
    pkv::database_pool pool(copy_from, 4, [&](size_t index) -> pkv::shared_db {
        const auto attempt = ++pool_creations;
        if (index == 2 && attempt == 2) return nullptr;
        auto created = pkv::db::create_mdb(2);
        return created->open("") ? created : nullptr;
    });
    require(pool.get(0) == copy_from && !pool.get(4) && pool_creations == 0, "pool changed DB 0 or opened invalid index");
    std::vector<pkv::shared_db> pool_results(8);
    std::vector<std::thread> pool_threads;
    for (size_t i = 0; i < pool_results.size(); ++i) pool_threads.emplace_back([&, i] { pool_results[i] = pool.get(1); });
    for (auto& thread : pool_threads) thread.join();
    for (const auto& result : pool_results) require(result && result == pool_results[0], "pool published duplicate logical DB instances");
    require(pool_creations == 1 && !pool.get(2) && pool.get(2) && pool_creations == 3,
            "pool cached failed creation or failed to retry safely");
    counting_watcher old_observer;
    database_counter identity_observer;
    auto namespaced_observers = std::make_shared<pkv::db_watchers>(3);
    namespaced_observers->add_watcher(&old_observer);
    namespaced_observers->add_watcher(&identity_observer);
    auto namespaced_fixture = std::make_shared<fixture_db>();
    pkv::shared_db namespaced_db = namespaced_fixture;
    require(namespaced_db->open_observed("", namespaced_observers) && namespaced_db->set("key", "local") &&
            old_observer.sets == 0 && identity_observer.sets == 1 && identity_observer.last_database == 3, "legacy observer received DB N records without database identity");
    std::weak_ptr<pkv::db_watchers> observer_lifetime = namespaced_observers;
    namespaced_observers.reset();
    require(!observer_lifetime.expired(), "database did not retain its observers");
    auto apply_databases = std::make_shared<pkv::database_pool>(copy_from, 4, [&](size_t) { return namespaced_db; });
    pkv::cluster_client multi_applier("", copy_from, apply_databases);
    require(multi_applier.apply(std::make_shared<pkv::kv_message>("", "", pkv::message_oper_ready)), "replica READY rejected");
    const auto remote = pkv::native_string_fixture("remote");
    namespaced_fixture->fail_write = true;
    require(!multi_applier.apply(std::make_shared<pkv::kv_message>("key", remote, pkv::message_oper_set, 3)) &&
            namespaced_fixture->records.at("key") == "local", "replica apply hid failed DB N write");
    namespaced_fixture->fail_write = false;
    require(multi_applier.apply(std::make_shared<pkv::kv_message>("key", remote, pkv::message_oper_set, 3)) &&
            namespaced_fixture->records.at("key") == remote && !copy_from_fixture->records.count("key") && old_observer.sets == 0 && identity_observer.sets == 1,
            "replica DB N write crossed keyspace or echoed an observer event");
    namespaced_fixture->fail_delete = true;
    require(!multi_applier.apply(std::make_shared<pkv::kv_message>("key", "", pkv::message_oper_del, 3)), "replica delete hid backend failure");
    namespaced_fixture->fail_delete = false;
    require(multi_applier.apply(std::make_shared<pkv::kv_message>("key", "", pkv::message_oper_del, 3)) &&
            multi_applier.apply(std::make_shared<pkv::kv_message>("key", "", pkv::message_oper_del, 3)) && old_observer.deletions == 0 && identity_observer.deletions == 0,
            "replica delete was not idempotent or echoed an event");
    std::cout << "Storage regression tests passed\n";
}
