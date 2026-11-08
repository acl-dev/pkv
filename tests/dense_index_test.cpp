#include "stdafx.h"
#include <functional>
#include "common/dense_key_index.h"
#include "db/page_transaction.h"
#include <array>
#include <cstdlib>
#include <iostream>
#include <new>
#include <set>
#include <thread>

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
static void require(bool ok, const char* reason) {
    if (!ok) { std::cerr << reason << '\n'; std::exit(1); }
}
static void metadata() {
    struct record { mutable size_t position = 0; };
    using node = std::pair<std::string, record>;
    std::array<node, 513> nodes;
    dense_key_index<node> index;
    unsigned rejected = 0;
    for (size_t i = 0; i < nodes.size(); ++i) {
        bool failed = false;
        budget = 0;
        try { index.prepare(); index.cancel(); }
        catch (const std::bad_alloc&) { failed = true; }
        budget = -1;
        rejected += failed;
        require(index.size() == i, "failed slot changed size");
        index.prepare(); index.publish(&nodes[i]);
        require(nodes[i].second.position == i, "published position");
    }
    require(rejected, "growth allocation never injected");
    // Backshift arbitrary positions while every key address remains stable.
    budget = 0;
    while (index.size()) index.erase(index[index.size() / 3]);
    budget = -1;
    index.prepare(); index.publish(&nodes[0]); index.erase(&nodes[0]);
    budget = 0;
    for (unsigned i = 0; i < 200; ++i) {
        index.prepare(); index.publish(&nodes[0]); index.erase(&nodes[0]);
    }
    budget = -1;
    require(!index.size(), "small churn retained a node");
}
static std::set<std::string> checked(shared_db db) {
    std::set<std::string> scanned, ranked;
    unsigned long long cursor = 0, next = 0;
    do {
        std::vector<std::string> page;
        require(db->scan_keys(cursor, 1, page, next) == db::scan_status::ok,
            "dense scan");
        scanned.insert(page.begin(), page.end()); cursor = next;
    } while (cursor);
    uint64_t count = 0;
    require(db->key_count(count) == db::count_status::ok &&
        count == scanned.size(), "dense count disagrees with root maps");
    for (uint64_t i = 0; i < count; ++i) {
        std::string key; bool found = false;
        require(db->key_at_rank(i, key, found) == db::scan_status::ok &&
            found && ranked.insert(key).second,
            "duplicate/dangling dense rank");
    }
    require(scanned == ranked, "dense index omitted or invented a root");
    return scanned;
}
static shared_db create(bool tbb) {
    auto db = tbb ? db::create_mdb_tbb(4) : db::create_mdb(4);
    require(db->open(""), "dense open"); return db;
}
static std::vector<std::string> collisions(bool tbb) {
    std::vector<std::string> keys;
    for (unsigned i = 0; keys.size() < 96; ++i) {
        const auto key = "dense:" + std::to_string(i);
        const auto hash = tbb ? acl_hash_crc32(key.data(), key.size()) :
            std::hash<std::string>()(key);
        if (hash % 1024 == 0)
            keys.push_back(key);
    }
    return keys;
}
static void growth(bool tbb, const std::vector<std::string>& keys) {
    auto db = create(tbb);
    std::set<std::string> expected;
    for (const auto& key : keys) {
        require(db->set(key, "before"), "dense grow"); expected.insert(key);
    }
    for (unsigned round = 0; round < 4; ++round) {
        for (size_t i = round % 3; i < keys.size(); i += 3) {
            const bool removed = expected.erase(keys[i]) != 0;
            require(db->del(keys[i]) == removed, "dense deletion");
            require(checked(db) == expected,
                "backshift changed key population");
        }
        for (const auto& key : keys) {
            require(db->set(key, "after"), "dense reuse"); expected.insert(key);
        }
        require(checked(db) == expected, "dense reinsert lost roots");
    }
}
static void faults(bool tbb, const std::vector<std::string>& keys) {
    unsigned failed = 0, succeeded = 0;
    for (unsigned transaction = 0; transaction < 2; ++transaction) {
        const auto previous_failed = failed, previous_succeeded = succeeded;
        for (long allowance = 0; allowance < 96; ++allowance) {
            auto db = create(tbb);
            std::set<std::string> before;
            for (size_t i = 0; i < 4; ++i) {
                require(db->set(keys[i], "before"), "dense OOM seed");
                before.insert(keys[i]);
            }
            const std::vector<std::string> selected(keys.begin(),
                keys.begin() + 24);
            bool ok = false;
            budget = allowance;
            try {
                if (!transaction) ok = db->set(keys[4], "after");
                else ok = db->transact(selected, [&](db::batch_view roots) {
                    for (auto& root : roots) {
                        root.action = root.key == keys[0] ?
                            db::update_action::erase : db::update_action::write;
                        if (root.action == db::update_action::write)
                            root.value = "after";
                    }
                    return true;
                }) == db::batch_status::ok;
            } catch (const std::bad_alloc&) {}
            budget = -1;
            auto expected = before;
            if (ok) {
                ++succeeded;
                if (transaction) {
                    expected.insert(selected.begin(), selected.end());
                    expected.erase(keys[0]);
                } else expected.insert(keys[4]);
            } else ++failed;
            require(checked(db) == expected,
                "OOM index/root rollback mismatch");
            for (const auto& key : expected) {
                std::string value;
                const bool changed = ok && (transaction || key == keys[4]);
                require(db->get(key, value) && value ==
                    (changed ? "after" : "before"), "OOM changed root bytes");
            }
        }
        require(failed > previous_failed && succeeded > previous_succeeded,
            "dense operation lacks failed/successful budgets");
    }
    require(failed && succeeded, "dense OOM coverage");
    std::cout << "Dense allocation budgets " << (tbb ? "tbb" : "mdb") <<
        ": failed=" << failed << " succeeded=" << succeeded << '\n';
}
static void concurrent(bool tbb, const std::vector<std::string>& keys) {
    auto db = create(tbb);
    const std::set<std::string> allowed(keys.begin(), keys.end());
    std::vector<std::thread> writers;
    for (unsigned worker = 0; worker < 4; ++worker)
        writers.emplace_back([&, worker] {
            for (unsigned round = 0; round < 30; ++round) {
                for (size_t i = worker; i < 48; i += 4)
                    require(db->set(keys[i], "v"), "dense concurrent insert");
                for (size_t i = worker; i < 48; i += 4)
                    require(db->del(keys[i]), "dense concurrent erase");
            }
        });
    for (unsigned i = 0; i < 500; ++i) {
        unsigned long long next = 0;
        std::vector<std::string> page;
        require(db->scan_keys(0, 1, page, next) == db::scan_status::ok,
            "dense concurrent SCAN");
        for (const auto& key : page)
            require(allowed.count(key), "SCAN dereferenced an invalid node");
        std::string key; bool found = false;
        require(db->sample_key(key, found) == db::scan_status::ok &&
            (!found || allowed.count(key)), "concurrent dense rank lifetime");
    }
    for (auto& writer : writers) writer.join();
    require(checked(db).empty(), "concurrent erase leaked a dense node");
}
int main() {
    acl::acl_cpp_init();
    metadata();
    for (bool tbb : {false, true}) {
#ifndef HAS_TBB
        if (tbb) continue;
#endif
        const auto keys = collisions(tbb);
        growth(tbb, keys); faults(tbb, keys); concurrent(tbb, keys);
    }
    std::cout << "Dense growth, arbitrary erase, reuse and rollback passed\n";
}
