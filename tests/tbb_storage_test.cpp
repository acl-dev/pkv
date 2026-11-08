#include "stdafx.h"
#include "db/db.h"
#include "db/list_store.h"
#include "db/map_store.h"
#include "dao/paged_hash.h"
#include "dao/paged_list.h"
#include "dao/json/json_key.h"
#include "common/redis_integer.h"
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <new>
#include <set>
#include <thread>

static thread_local long budget = -1;
void* operator new(std::size_t size) {
    if (!budget) throw std::bad_alloc();
    if (budget > 0) --budget;
    if (void* value = std::malloc(size ? size : 1)) return value;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* value) noexcept { std::free(value); }
void operator delete[](void* value) noexcept { std::free(value); }
#if __cplusplus >= 201402L
void operator delete(void* value, std::size_t) noexcept { std::free(value); }
void operator delete[](void* value, std::size_t) noexcept { std::free(value); }
#endif

using namespace pkv;
static void require(bool ok, const char* reason) {
    if (!ok) { std::cerr << reason << '\n'; std::exit(1); }
}

static shared_db memory(size_t count = 8) {
    auto database = db::create_mdb_tbb(count);
    require(database && database->open(""), "oneTBB open");
    return database;
}

static std::set<std::string> keys(shared_db database, size_t count = 1) {
    std::set<std::string> found;
    unsigned long long cursor = 0;
    do {
        std::vector<std::string> page;
        require(database->scan_keys(cursor, count, page, cursor) ==
            db::scan_status::ok, "native key scan");
        found.insert(page.begin(), page.end());
    } while (cursor);
    return found;
}

static void test_binary_and_raw_failures() {
    auto database = memory(0);
    std::string binary;
    for (unsigned i = 0; i < 256; ++i) binary += static_cast<char>(i);
    for (const auto& key : {std::string(), binary}) {
        std::string out;
        require(database->set(key, binary) && database->get(key, out) &&
            out == binary && keys(database).count(key), "binary raw key/value");
    }
    for (bool replace : {false, true}) {
        bool success = false;
        unsigned failures = 0;
        const std::string key(100, 'k'), value(1000, 'v');
        for (long limit = 0; limit < 300; ++limit) {
            auto fixture = memory(1);
            if (replace) require(fixture->set(key, "before"), "raw old value");
            budget = limit;
            bool ok = fixture->set(key, value);
            budget = -1;
            std::string out;
            if (ok) { success = true; break; }
            ++failures;
            const bool found = fixture->get(key, out);
            const auto indexed = keys(fixture).size();
            if (found != replace || (replace && out != "before") ||
                indexed != (replace ? 1u : 0u))
                std::cerr << "replace=" << replace << " budget=" << limit <<
                    " found=" << found << " bytes=" << out.size() <<
                    " indexed=" << indexed << '\n';
            require(found == replace && (!replace || out == "before") &&
                indexed == (replace ? 1 : 0),
                "failed SET changed value or key index");
            require(fixture->set(key, value) && fixture->get(key, out) &&
                out == value && keys(fixture) == std::set<std::string>{key},
                "SET could not recover after failed oneTBB growth");
        }
        require(success && failures, "raw SET allocation sweep");
    }
}

static void test_batch_rollback() {
    bool success = false;
    unsigned failures = 0;
    for (long limit = 0; limit < 1200; ++limit) {
        auto database = memory(4);
        std::vector<std::string> names;
        std::set<std::string> original;
        for (unsigned i = 0; i < 24; ++i) {
            names.push_back(std::string(80, 'k') + std::to_string(i));
            if (i < 16) {
                require(database->set(names.back(), "before"), "batch seed");
                original.insert(names.back());
            }
        }
        const auto result = database->transact(names,
            [&](db::batch_view roots) {
            unsigned i = 0;
            for (auto& root : roots) {
                root.action = i++ < 3 ? db::update_action::erase :
                    db::update_action::write;
                root.value.assign(1000, 'v');
            }
            budget = limit;
            return true;
        });
        budget = -1;
        if (result == db::batch_status::ok) {
            auto ordered = names;
            std::sort(ordered.begin(), ordered.end());
            for (size_t i = 0; i < ordered.size(); ++i) {
                std::string value;
                const bool found = database->get(ordered[i], value);
                require(i < 3 ? !found : found && value ==
                    std::string(1000, 'v'),
                    "successful batch omitted root changes");
            }
            success = true; break;
        }
        require(result == db::batch_status::error, "unexpected batch result");
        ++failures;
        require(keys(database) == original, "failed batch changed key index");
        for (const auto& key : original) {
            std::string out;
            require(database->get(key, out) && out == "before",
                "batch exposed a partial root");
        }
    }
    require(success && failures > 20, "batch allocation sweep");
}

static void seed_pages(shared_db database) {
    require(database->transact_pages({"a", "b"}, [](page_transaction& tx) {
        for (auto& root : tx.roots()) {
            root.value = "before"; root.action = db::update_action::write;
            if (!tx.put(root.key, 1, "old-page")) return false;
        }
        return tx.put("a", 2, "second");
    }) == db::batch_status::ok, "page seed");
}

static void check_pages(shared_db database) {
    require(keys(database) == std::set<std::string>{"a", "b"}, "page index");
    require(database->transact_pages({"a", "b", "new"},
        [](page_transaction& tx) {
        for (const auto& name : {"a", "b"}) {
            const auto* root = tx.roots().find(name);
            std::string bytes;
            require(root->found && root->value == "before" &&
                tx.lookup(name, 1, bytes) == db::lookup_status::found &&
                bytes == "old-page", "page rollback changed existing owner");
        }
        std::string bytes;
        return tx.lookup("a", 2, bytes) == db::lookup_status::found &&
            bytes == "second" && tx.lookup("a", 3, bytes) ==
                db::lookup_status::missing && tx.lookup("new", 1, bytes) ==
                db::lookup_status::missing;
    }) == db::batch_status::ok, "page rollback");
}

static void test_page_reset_rollback() {
    bool success = false;
    unsigned failures = 0;
    for (long limit = 0; limit < 500; ++limit) {
        auto database = memory();
        seed_pages(database);
        const auto result = database->transact_pages({"a", "b", "new"},
            [&](page_transaction& tx) {
            auto& a = *tx.roots().find("a");
            auto& b = *tx.roots().find("b");
            auto& fresh = *tx.roots().find("new");
            a.value = "after"; a.action = db::update_action::write;
            b.action = db::update_action::erase;
            fresh.value = "created"; fresh.action = db::update_action::write;
            if (!tx.reset_pages("a") || !tx.put("a", 3, "replacement") ||
                !tx.erase("b", 1) || !tx.put("new", 1, "fresh")) return false;
            budget = limit;
            return true;
        });
        budget = -1;
        if (result == db::batch_status::ok) {
            require(keys(database) == std::set<std::string>{"a", "new"},
                "successful reset index");
            require(database->transact_pages({"a", "b", "new"},
                [](page_transaction& tx) {
                std::string bytes;
                return tx.roots().find("a")->value == "after" &&
                    !tx.roots().find("b")->found &&
                    tx.roots().find("new")->value == "created" &&
                    tx.lookup("a", 1, bytes) == db::lookup_status::missing &&
                    tx.lookup("a", 2, bytes) == db::lookup_status::missing &&
                    tx.lookup("a", 3, bytes) == db::lookup_status::found &&
                    bytes == "replacement" && tx.lookup("b", 1, bytes) ==
                        db::lookup_status::missing &&
                    tx.lookup("new", 1, bytes) == db::lookup_status::found &&
                    bytes == "fresh";
            }) == db::batch_status::ok, "successful reset omitted changes");
            success = true; break;
        }
        ++failures;
        require(result == db::batch_status::error, "page native result");
        check_pages(database);
    }
    require(success && failures > 10, "page reset allocation sweep");
}

static void test_single_page_resize_rollback() {
    auto database = memory();
    seed_pages(database);
    const std::string replacement(4096, 'p');
    bool failed = false;
    bool committed = false;
    for (long limit = 0; limit < 32 && !committed; ++limit) {
        const auto result = database->transact_page("a", [&](page_transaction& tx) {
            auto& root = *tx.roots().find("a");
            root.value = "update";
            root.action = db::update_action::write;
            if (!tx.put("a", 1, replacement)) return false;
            budget = limit;
            return true;
        });
        budget = -1;
        committed = result == db::batch_status::ok;
        if (!committed) {
            require(result == db::batch_status::error, "single page resize result");
            failed = true;
            check_pages(database);
        }
    }
    require(failed && committed, "single page resize allocation sweep");
    require(database->transact_page("a", [&](page_transaction& tx) {
        std::string bytes;
        return tx.roots().find("a")->value == "update" &&
            tx.lookup("a", 1, bytes) == db::lookup_status::found &&
            bytes == replacement && tx.lookup("a", 2, bytes) ==
                db::lookup_status::found && bytes == "second";
    }) == db::batch_status::ok, "single page resize publication");
}

static void test_scan_churn() {
    auto database = memory(4);
    std::set<std::string> stable;
    for (unsigned i = 0; i < 300; ++i) {
        const auto key = std::string("stable\0", 7) + std::to_string(i);
        stable.insert(key);
        require(database->set(key, "v"), "stable seed");
    }
    std::atomic<bool> start(false);
    std::atomic<unsigned> done(0);
    std::vector<std::thread> threads;
    for (unsigned t = 0; t < 4; ++t) threads.emplace_back([&, t] {
        while (!start.load()) std::this_thread::yield();
        for (unsigned i = 0; i < 500; ++i) {
            const auto key = "volatile:" + std::to_string(t) + ":" +
                std::to_string(i);
            require(database->set(key, "v"), "churn SET");
            if (i) require(database->del("volatile:" + std::to_string(t) +
                ":" + std::to_string(i - 1)), "churn DEL");
        }
        ++done;
    });
    start = true;
    unsigned passes = 0;
    do {
        const auto seen = keys(database);
        for (const auto& key : stable)
            require(seen.count(key), "native SCAN skipped stable key");
        ++passes;
    } while (done != 4 || passes < 3);
    for (auto& thread : threads) thread.join();
    const auto snapshot = keys(database);
    bool success = false;
    unsigned failures = 0;
    for (long limit = 0; limit < 100; ++limit) {
        std::vector<std::string> page = {"sentinel"};
        unsigned long long next = 99;
        budget = limit;
        const auto result = database->scan_keys(0, 10, page, next);
        budget = -1;
        if (result == db::scan_status::ok) { success = true; break; }
        ++failures;
        require(result == db::scan_status::error && page.empty() && !next &&
            keys(database) == snapshot, "failed SCAN leaked partial state");
    }
    require(success && failures, "native scan allocation sweep");
}

static void test_commands_and_yielding_transactions() {
    auto database = memory(), target = memory();
    dao::paged_hash hash;
    dao::paged_list list;
    dao::json_key key;
    long long count;
    require(hash.hset(database, "hash", "count", "0") &&
        list.push(database, "list", {"base"}, false, false, count), "DAO seed");
    std::vector<std::thread> threads;
    for (unsigned t = 0; t < 4; ++t) threads.emplace_back([&, t] {
        for (unsigned f = 0; f < 4; ++f) go[&, t, f] {
            dao::paged_hash hashes;
            for (unsigned i = 0; i < 20; ++i) {
                long long value;
                require(hashes.hincrby(database, "hash", "count", 1, value),
                    "oneTBB HINCRBY");
                const auto token = std::to_string(t) + ":" + std::to_string(f);
                require(database->transact_pages({"hash", "list"},
                    [&](page_transaction& tx) {
                    map_store fields(tx, "hash");
                    list_store values(tx, "list");
                    std::string bytes;
                    bool found, added;
                    if (!fields.open() || !values.open() ||
                        !fields.get("count", bytes, found) || !found ||
                        !parse_redis_integer(bytes, value) ||
                        !fields.put("count", std::to_string(value + 1), false,
                            added) || !values.push({token}, true)) return false;
                    acl::fiber::yield();
                    std::vector<std::string> out;
                    return values.pop(1, true, &out) && out ==
                        std::vector<std::string>{token} && fields.finish() &&
                            values.finish();
                }) == db::batch_status::ok, "yielding multi-key page commit");
            }
        };
        acl::fiber::schedule();
    });
    for (auto& thread : threads) thread.join();
    std::string value;
    require(hash.hget(database, "hash", "count", value) && value == "640",
        "lost concurrent increments");
    std::vector<std::string> members;
    require(list.range(database, "list", 0, -1, members) &&
        members == std::vector<std::string>{"base"}, "lost List members");
    bool copied;
    require(key.set_expiry(database, "hash", 4102444800123LL, 0) &&
        key.copy(database, "hash", target, "hash", true, copied) && copied &&
        hash.hget(target, "hash", "count", value) && value == "640" &&
        key.rename(target, "hash", "renamed", false, copied) && copied &&
        key.expiry_time(target, "renamed", count) && count == 4102444800123LL,
        "oneTBB TTL/COPY/RENAME");
}

int main() {
    acl::acl_cpp_init();
    test_binary_and_raw_failures();
    test_batch_rollback();
    test_page_reset_rollback();
    test_single_page_resize_rollback();
    test_scan_churn();
    test_commands_and_yielding_transactions();
    std::cout << "oneTBB native pages, rollback and concurrent SCAN passed\n";
}
