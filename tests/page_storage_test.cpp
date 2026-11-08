#include "stdafx.h"
#include "db/db.h"
#include "db/db_watcher.h"
#include "db/mdb/mdb.h"
#include "db/page_transaction.h"
#include "db/root_workspace.h"
#include "coder/list_page.h"
#include <atomic>
#include <functional>
#include <cstdlib>
#include <iostream>
#include <new>
#include <set>
#include <stdexcept>
#include <thread>

// Fault injection covers staging, native preparation and native insertion.
static thread_local long allocation_budget = -1;
static thread_local bool measure_allocations = false;
static thread_local size_t allocated_bytes = 0;
void* operator new(std::size_t size) {
    if (allocation_budget == 0) {
        throw std::bad_alloc();
    }
    if (allocation_budget > 0) {
        --allocation_budget;
    }
    if (measure_allocations) {
        allocated_bytes += size;
    }
    if (void* pointer = std::malloc(size ? size : 1)) {
        return pointer;
    }
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
#if __cplusplus >= 201402L
void operator delete(void* pointer, std::size_t) noexcept {
    std::free(pointer);
}
void operator delete[](void* pointer, std::size_t) noexcept {
    std::free(pointer);
}
#endif

using namespace pkv;
using batch_status = db::batch_status;
using lookup_status = db::lookup_status;
using action = db::update_action;

static void require(bool ok, const char* reason) {
    if (!ok) {
        std::cerr << reason << '\n';
        std::exit(1);
    }
}

static shared_db memory() {
    auto database = db::create_mdb(8);
    require(database->open(""), "memory open");
    return database;
}

static void put_root(page_transaction& transaction, const std::string& key,
    const std::string& value) {
    auto& root = *transaction.roots().find(key);
    root.value = value;
    root.action = action::write;
}

static std::string read_page(shared_db database, const std::string& owner,
    uint64_t id, bool found = true) {
    std::string out = "sentinel";
    require(database->transact_pages({owner}, [&](page_transaction& view) {
        const auto status = view.lookup(owner, id, out);
        require(status == (found ? lookup_status::found :
            lookup_status::missing), "page lookup status");
        return true;
    }) == batch_status::ok, "page read transaction");
    require(found || out == "sentinel", "missing page changed output");
    return out;
}

static void seed(shared_db database, const std::string& key, size_t count) {
    require(database->transact_pages({key}, [&](page_transaction& view) {
        put_root(view, key, "root");
        for (size_t i = 1; i <= count; ++i) {
            require(view.put(key, i, "page:" + std::to_string(i)),
                "seed page stage");
        }
        return true;
    }) == batch_status::ok, "seed pages");
}

static void test_scratch_reads() {
    auto database = memory();
    const std::string first(256, 'a'), second(1024, 'b');
    const std::string staged_first(512, 'c'), staged_second(2048, 'd');
    const std::string replacement(4096, 'e');
    require(database->transact_pages({"owner"}, [&](page_transaction& tx) {
        put_root(tx, "owner", "root");
        return tx.put("owner", 1, first) && tx.put("owner", 2, second);
    }) == batch_status::ok, "scratch seed");
    std::string snapshot;
    const auto status = database->transact_pages({"owner"},
        [&](page_transaction& tx) {
            std::string scratch;
            scratch.reserve(page_transaction::max_value_bytes);
            std::string alias = "owner";
            require(tx.lookup_scratch(alias, 1, alias) ==
                lookup_status::found && alias == first,
                "scratch owner/output alias");
            allocation_budget = 0;
            for (unsigned i = 0; i < 128; ++i) {
                const auto id = i % 2 + 1;
                require(tx.lookup_scratch("owner", id, scratch) ==
                    lookup_status::found && scratch ==
                    (id == 1 ? first : second), "native scratch capacity");
            }
            allocation_budget = -1;
            snapshot = scratch;
            require(tx.put("owner", 1, staged_first) &&
                tx.put("owner", 2, staged_second), "scratch stage");
            allocation_budget = 0;
            for (unsigned i = 0; i < 128; ++i) {
                const auto id = i % 2 + 1;
                require(tx.lookup_scratch("owner", id, scratch) ==
                    lookup_status::found && scratch ==
                    (id == 1 ? staged_first : staged_second),
                    "staged scratch capacity");
            }
            allocation_budget = -1;
            require(tx.erase("owner", 1) &&
                tx.lookup_scratch("owner", 1, scratch) ==
                    lookup_status::missing, "scratch staged deletion");
            require(tx.reset_pages("owner") &&
                tx.lookup_scratch("owner", 2, scratch) ==
                    lookup_status::missing, "scratch reset hides native");
            require(tx.put("owner", 3, replacement) &&
                tx.lookup_scratch("owner", 3, scratch) ==
                    lookup_status::found && scratch == replacement,
                    "scratch read after reset/stage");
            put_root(tx, "owner", "next");
            return true;
        });
    allocation_budget = -1;
    require(status == batch_status::ok && snapshot == second,
        "scratch snapshot ownership");
    require(read_page(database, "owner", 3) == replacement,
        "scratch commit bytes");
    read_page(database, "owner", 1, false);
    read_page(database, "owner", 2, false);
}

static void test_namespace_and_ownership() {
    auto database = memory();
    const std::string owner("\0PLP\1owner\xff", 11);
    seed(database, owner, 2);
    seed(database, "", 1);
    require(database->set("PLP\1", "ordinary"), "prefix key set");
    require(read_page(database, owner, 1) == "page:1" &&
        read_page(database, "", 1) == "page:1", "binary/empty owner");
    std::vector<std::string> keys;
    unsigned long long next = 99;
    require(database->scan_keys(0, 100, keys, next) == db::scan_status::ok &&
        next == 0 && std::set<std::string>(keys.begin(), keys.end()) ==
            std::set<std::string>({owner, "", "PLP\1"}),
        "hidden pages leaked into key namespace");
    std::string snapshot;
    require(database->transact_pages({owner}, [&](page_transaction& view) {
        require(view.lookup(owner, 1, snapshot) == lookup_status::found,
            "owning snapshot read");
        require(view.put(owner, 1, "new"), "stage overwrite");
        std::string staged;
        require(view.lookup(owner, 1, staged) == lookup_status::found &&
            staged == "new", "read own write");
        require(view.erase(owner, 2), "stage erase");
        require(view.lookup(owner, 2, staged) == lookup_status::missing &&
            staged == "new", "read own deletion");
        require(view.put(owner, 2, "revived"), "restage erased page");
        return true;
    }) == batch_status::ok, "staged overwrite commit");
    require(snapshot == "page:1" && read_page(database, owner, 1) == "new" &&
        read_page(database, owner, 2) == "revived", "page ownership");
    read_page(database, owner, 3, false);
    read_page(database, "PLP\1", 1, false);
}

static void test_moved_pages() {
    auto database = memory();
    const std::string original(16384, '\x80');
    require(database->transact_pages({"move"}, [&](page_transaction& tx) {
        put_root(tx, "move", "root");
        auto value = original;
        // Transferring one existing buffer must not allocate a second buffer
        // or a tree node. Commit bookkeeping is measured separately.
        allocation_budget = 0;
        const bool moved = tx.put("move", 1, std::move(value));
        allocation_budget = -1;
        require(moved, "first moved page stage");
        std::string out;
        require(tx.lookup("move", 1, out) == lookup_status::found &&
            out == original, "moved page read-own-write");
        std::string copied(100, 'c');
        require(tx.put("move", 2, copied), "copied page stage");
        copied.assign(100, 'z');
        require(tx.lookup("move", 2, out) == lookup_status::found &&
            out == std::string(100, 'c'), "lvalue input was borrowed");
        return true;
    }) == batch_status::ok, "zero-allocation moved stage");
    require(read_page(database, "move", 1) == original,
        "moved page commit bytes");
    const std::string name(200, 'a');
    require(database->transact_pages({name}, [&](page_transaction& tx) {
        put_root(tx, name, "root");
        std::string owner = name;
        return tx.put(owner, 1, std::move(owner));
    }) == batch_status::ok && read_page(database, name, 1) == name,
        "aliased owner moved before address was copied");
}

class ordered_pages_db : public mdb {
public:
    ordered_pages_db() : mdb(4) {}
    size_t observed = 0;
protected:
    batch_status dbapply_pages(const std::vector<batch_entry>& roots,
        const std::vector<page_change>& pages) override {
        for (size_t i = 1; i < pages.size(); ++i) {
            const auto& a = pages[i - 1];
            const auto& b = pages[i];
            require(a.owner < b.owner ||
                (a.owner == b.owner && a.id < b.id), "page release order");
        }
        observed += pages.size();
        return mdb::dbapply_pages(roots, pages);
    }
};

static void test_single_owner_staging_order() {
    for (uint64_t count : {2, 40, 41, 43}) {
        // The smallest batch also exercises IDs too wide for packed sorting.
        const uint64_t stride = count == 2 ? 1ULL << 60 : 1ULL << 18;
        auto engine = std::make_shared<ordered_pages_db>();
        shared_db database = engine;
        require(database->open(""), "open ordered page store");
        require(database->transact_page("owner", [&](page_transaction& tx) {
            put_root(tx, "owner", "root");
            std::string value;
            require(tx.put("owner", 1, "discarded") &&
                tx.put("owner", 1 + stride, "discarded") && tx.reset_pages("owner") &&
                tx.lookup("owner", 1 + stride, value) == lookup_status::missing,
                "discard staged pages and their lookup entries");
            for (auto i = count; i > 0; --i)
                require(tx.put("owner", 1 + stride * i,
                    std::string(48, 'v') + std::to_string(i)),
                    "stage colliding pages");
            require(tx.erase("owner", 1 + stride) &&
                tx.lookup("owner", 1 + stride, value) == lookup_status::missing &&
                tx.put("owner", 1 + stride, "restaged"), "replace staged deletion");
            return true;
        }) == batch_status::ok && engine->observed == count,
            "release ordered unique pages");
        require(read_page(database, "owner", 1 + stride) == "restaged",
            "commit restaged page");
        for (uint64_t i = 2; i <= count; ++i)
            require(read_page(database, "owner", 1 + stride * i) ==
                std::string(48, 'v') + std::to_string(i),
                "page contents followed the sorted ID");
    }
}

static void test_dense_page_prefix() {
    for (size_t slots : {304U, 320U, 321U, 512U}) {
        bool failed = false, prepared = false;
        for (long budget = 0; budget < 16 && !prepared; ++budget) {
            mdb_page_index index;
            std::array<uint64_t, 512> ids;
            for (size_t i = 0; i < ids.size(); ++i) ids[i] = i + 1;
            allocation_budget = budget;
            try {
                index.prepare_empty(ids.begin(), ids.begin() + slots,
                    [](uint64_t id) { return id; });
                prepared = true;
            } catch (const std::bad_alloc&) { failed = true; }
            allocation_budget = -1;
            require(index.empty(), "bulk suffix preparation published pages");
            if (!prepared) {
                index.emplace(1, "usable");
                require(index.get(1) && *index.get(1) == "usable",
                    "bulk preparation failure poisoned the index");
                continue;
            }
            allocated_bytes = 0;
            measure_allocations = true;
            for (size_t i = 0; i < std::min(slots, size_t(320)); ++i)
                index.emplace(ids[i], "bulk");
            measure_allocations = false;
            require(allocated_bytes == 0,
                "known dense bulk suffix allocated per-page hash nodes");
            const auto* pinned = index.get(257);
            for (size_t i = 320; i < slots; ++i) index.emplace(ids[i], "bulk");
            require(index.size() == slots && index.get(257) == pinned,
                "bulk suffix growth moved an existing page");
            for (size_t i = 0; i < slots; ++i)
                require(index.get(ids[i]) && *index.get(ids[i]) == "bulk",
                    "bulk suffix preparation lost contents");
        }
        require(failed && prepared, "bulk suffix preparation fault sweep incomplete");
    }
    {
        mdb_page_index index;
        std::array<uint64_t, 256> ids;
        for (size_t i = 0; i < ids.size(); ++i) ids[i] = i + 1;
        index.prepare_empty(ids.begin(), ids.end(), [](uint64_t id) { return id; });
        for (uint64_t id : ids) index.emplace(id, "prefix");
        allocated_bytes = 0;
        measure_allocations = true;
        index.emplace(257, "suffix");
        measure_allocations = false;
        require(allocated_bytes < 512, "one suffix page allocated a dense 64-slot array");
        const auto* suffix = index.get(257);
        for (uint64_t id = 258; id <= 320; ++id) {
            index.emplace(id, "suffix");
            require(index.get(257) == suffix, "suffix insertion moved a pinned page");
        }
        index.promote_tail(320);
        require(index.get(257) != suffix && index.size() == 320,
            "dense suffix was not promoted after publication");
        for (uint64_t id = 1; id <= 320; ++id)
            require(index.get(id) && *index.get(id) == (id <= 256 ? "prefix" : "suffix"),
                "deferred suffix promotion changed contents");
    }
    for (size_t slots : {241U, 249U, 255U}) {
        bool failed = false, completed = false;
        for (long budget = 0; budget < 16 && !completed; ++budget) {
            mdb_page_index index;
            std::array<uint64_t, 256> ids;
            for (size_t i = 0; i < ids.size(); ++i) ids[i] = i + 1;
            index.prepare_empty(ids.begin(), ids.begin() + slots,
                [](uint64_t id) { return id; });
            for (uint64_t id : ids) index.emplace(id, std::to_string(id));
            // Keep an unrelated sparse entry and its address through promotion.
            index.emplace(UINT64_MAX, "far");
            const auto* far = index.get(UINT64_MAX);
            const auto* last = index.get(256);
            allocation_budget = budget;
            index.promote_prefix(1);
            allocation_budget = -1;
            completed = index.get(256) != last;
            failed = failed || !completed;
            require(index.size() == 257 && index.get(UINT64_MAX) == far &&
                *far == "far", "short prefix completion changed sparse ownership");
            for (uint64_t id : ids)
                require(index.get(id) && *index.get(id) == std::to_string(id),
                    "short prefix completion lost a page on allocation failure");
        }
        require(failed && completed, "short prefix completion fault sweep incomplete");
        mdb_page_index index;
        std::array<uint64_t, 256> ids;
        for (size_t i = 0; i < ids.size(); ++i) ids[i] = i + 1;
        index.prepare_empty(ids.begin(), ids.begin() + slots,
            [](uint64_t id) { return id; });
        for (uint64_t id : ids) if (id != 250) index.emplace(id, std::to_string(id));
        for (uint64_t id = 257; id <= 320; ++id) {
            index.emplace(id, "tail");
            index.promote_prefix(1);
            index.promote_tail(id);
        }
        require(!index.get(250) && index.size() == 319 &&
            index.get(320) && *index.get(320) == "tail",
            "short prefix completion filled a hole or lost the tail");
    }
    {
        std::array<uint64_t, 256> ids;
        for (size_t i = 0; i < ids.size(); ++i) ids[i] = i + 1;
        const std::string payload(40, 'p');
        for (size_t slots : {16U, 17U, 31U, 32U, 63U, 64U,
                127U, 128U, 255U, 256U}) {
            bool failed = false, prepared = false;
            for (long budget = 0; budget < 16 && !prepared; ++budget) {
                mdb_page_index index;
                allocation_budget = budget;
                try {
                    index.prepare_empty(ids.begin(), ids.begin() + slots,
                        [](uint64_t id) { return id; });
                    prepared = true;
                } catch (const std::bad_alloc&) { failed = true; }
                allocation_budget = -1;
                require(index.empty(), "prefix preparation published pages");
                if (!prepared)
                    index.prepare_empty(ids.begin(), ids.begin() + slots,
                        [](uint64_t id) { return id; });
                for (size_t i = 0; i < slots; ++i) index.emplace(ids[i], payload);
                const auto* pinned = index.get(slots);
                require(pinned && *pinned == payload, "prefix construction lost payload");
                for (size_t i = 1; i < slots; ++i) index.erase(i, true);
                require(index.size() == 1 && index.get(slots) == pinned &&
                    *pinned == payload, "partial prefix deletion moved a live page");
                index.emplace(UINT64_MAX, "far");
                const auto* far = index.get(UINT64_MAX);
                index.erase(slots, true);
                require(index.size() == 1 && !index.get(slots) &&
                    index.get(UINT64_MAX) == far, "prefix release lost a sparse page");
                for (size_t i = 0; i < slots; ++i) {
                    index.emplace(ids[i], payload);
                    index.promote_prefix(1);
                }
                size_t visited = 0;
                require(index.visit([&](uint64_t id, const std::string& value) {
                    ++visited;
                    return id == UINT64_MAX ? value == "far" :
                        id >= 1 && id <= slots && value == payload;
                }) && visited == slots + 1 && index.get(UINT64_MAX) == far,
                    "regrown prefix ownership or sparse address changed");
            }
            require(failed && prepared, "prefix allocation fault sweep incomplete");
        }
    }
    {
        const uint64_t first = (uint64_t(1) << 48) + 1;
        bool failed = false, promoted = false;
        for (long budget = 0; budget < 16 && !promoted; ++budget) {
            mdb_page_index index;
            for (uint64_t id = first; id < first + 64; ++id)
                index.emplace(id, std::to_string(id));
            const auto* address = index.get(first);
            allocation_budget = budget;
            index.promote_tail(first + 63);
            allocation_budget = -1;
            promoted = index.get(first) != address;
            failed = failed || !promoted;
            require(index.size() == 64, "remote promotion changed count on failure");
            for (uint64_t id = first; id < first + 64; ++id)
                require(index.get(id) && *index.get(id) == std::to_string(id),
                    "remote promotion failure lost a page");
        }
        require(failed && promoted, "remote promotion fault sweep incomplete");
        mdb_page_index index;
        for (uint64_t id = 1; id <= 32; ++id) {
            index.emplace(id, std::to_string(id)); index.promote_prefix(1);
        }
        for (uint64_t id = first; id < first + 64; ++id)
            index.emplace(id, std::to_string(id));
        allocated_bytes = 0; measure_allocations = true;
        index.promote_tail(first + 63);
        measure_allocations = false;
        require(allocated_bytes < 16384, "remote promotion allocated by page span");
        const auto* pinned = index.get(first);
        const auto* payload = pinned->data();
        for (uint64_t id = 1; id <= 32; ++id) index.erase(id, true);
        require(index.size() == 64 && index.get(first) == pinned,
            "empty prefix discarded remote blocks");
        for (uint64_t id = 1; id <= 32; ++id) index.emplace(id, std::to_string(id));
        index.promote_prefix(32);
        for (uint64_t block = 1; block <= 512; ++block) {
            const auto start = first + block * 4096;
            for (uint64_t id = start; id < start + 64; ++id)
                index.emplace(id, std::to_string(id));
            index.promote_tail(start + 63);
            require(index.get(first) == pinned && pinned->data() == payload,
                "remote directory rehash moved a pinned page");
        }
        const auto selected = first + 4096;
        for (uint64_t id = selected; id < selected + 56; ++id) index.erase(id, true);
        for (uint64_t id = selected + 56; id < selected + 64; ++id)
            require(index.get(id) && *index.get(id) == std::to_string(id),
                "remote block release lost survivors");
        for (uint64_t id = selected; id < selected + 56; ++id)
            index.emplace(id, std::to_string(id));
        index.promote_tail(selected + 63);
        size_t visited = 0;
        require(index.visit([&](uint64_t id, const std::string& value) {
            ++visited; return value == std::to_string(id);
        }) && visited == 32 + 513 * 64 && index.size() == visited,
            "remote prefix/release/regrowth visit mismatch");
        mdb_page_index other;
        other.swap(index);
        require(index.empty() && other.get(first) == pinned && pinned->data() == payload,
            "remote family swap moved a pinned page");
        allocation_budget = 0;
        for (uint64_t block = 1; block <= 512; ++block)
            for (uint64_t id = first + block * 4096; id < first + block * 4096 + 64; ++id)
                other.erase(id, true);
        allocation_budget = -1;
        require(other.size() == 96 && other.get(first) == pinned &&
            pinned->data() == payload, "remote contraction failure moved a live page");
        const auto temporary = first + 1024 * 4096;
        for (uint64_t id = temporary; id < temporary + 64; ++id)
            other.emplace(id, std::to_string(id));
        other.promote_tail(temporary + 63);
        for (uint64_t id = temporary; id < temporary + 64; ++id) other.erase(id, true);
        require(other.size() == 96 && other.get(first) == pinned &&
            pinned->data() == payload, "remote contraction success moved a live page");
        for (uint64_t id = 1; id <= 32; ++id) other.erase(id, true);
        for (uint64_t id = first; id < first + 64; ++id) other.erase(id, true);
        require(other.empty() && !other.get(first), "remote deletion retained pages");
        const auto last = UINT64_MAX & ~uint64_t(63);
        for (uint64_t id = last - 63; id <= last; ++id)
            other.emplace(id, std::to_string(id));
        other.promote_tail(last);
        visited = 0;
        require(other.visit([&](uint64_t id, const std::string& value) {
            ++visited; return id >= last - 63 && id <= last && value == std::to_string(id);
        }) && visited == 64 && !other.get(UINT64_MAX),
            "near-maximum remote page IDs wrapped during visit");
    }
    {
        bool failed = false, completed = false;
        for (long budget = 0; budget < 16 && !completed; ++budget) {
            mdb_page_index index;
            for (uint64_t id = 1; id <= 320; ++id) {
                index.emplace(id, std::to_string(id));
                index.promote_prefix(1);
                index.promote_tail(id);
            }
            const auto* pinned = index.get(257);
            const auto* payload = pinned->data();
            allocation_budget = budget;
            try { index.emplace(321, "new block"); completed = true; }
            catch (const std::bad_alloc&) { failed = true; }
            allocation_budget = -1;
            require(index.get(257) == pinned && pinned->data() == payload &&
                *pinned == "257" && index.size() == (completed ? 321 : 320) &&
                (completed ? index.get(321) && *index.get(321) == "new block" :
                    !index.get(321)), "directory growth failure moved a pinned page");
        }
        require(failed && completed, "directory growth fault sweep incomplete");
        mdb_page_index index;
        for (uint64_t id = 1; id <= 320; ++id) {
            index.emplace(id, std::to_string(id));
            index.promote_prefix(1);
            index.promote_tail(id);
        }
        const auto* pinned = index.get(257);
        const auto* payload = pinned->data();
        for (uint64_t id = 321; id <= 65536; ++id) {
            index.emplace(id, std::to_string(id));
            if (id % 1024 == 0)
                require(index.get(257) == pinned && pinned->data() == payload,
                    "directory relocation moved live string storage");
        }
        for (uint64_t id = 321; id <= 65536; ++id) index.erase(id, true);
        mdb_page_index other;
        other.swap(index);
        require(index.empty() && other.size() == 320 &&
            other.get(257) == pinned && pinned->data() == payload && *pinned == "257",
            "directory contraction or family swap moved a pinned page");
    }
    {
        mdb_page_index index;
        for (uint64_t id = 1; id <= 320; ++id) {
            index.emplace(id, std::to_string(id));
            index.promote_prefix(1);
            index.promote_tail(id);
        }
        index.emplace(4096, "");
        for (uint64_t id = 1; id <= 256; ++id) index.erase(id, true);
        require(index.size() == 65 && !index.get(256) &&
            index.get(257) && *index.get(257) == "257" &&
            index.get(4096) && index.get(4096)->empty(),
            "empty prefix reclamation preserves tail and sparse pages");
        for (uint64_t id = 1; id <= 32; ++id) index.emplace(id, "regrown");
        index.promote_prefix(32);
        require(index.size() == 97 && index.get(32) &&
            *index.get(32) == "regrown" && index.get(320) && *index.get(320) == "320",
            "regrow a released prefix without disturbing the tail");
    }
    {
        mdb_page_index index;
        for (uint64_t id = 1; id <= 384; ++id) {
            index.emplace(id, std::to_string(id));
            index.promote_prefix(1);
            index.promote_tail(id);
        }
        const auto address = reinterpret_cast<uintptr_t>(index.get(320));
        for (uint64_t id = 257; id <= 312; ++id) index.erase(id, true);
        require(index.size() == 328 && !index.get(312) && index.get(320) &&
            reinterpret_cast<uintptr_t>(index.get(320)) != address,
            "release nearly empty dense tail block");
        for (uint64_t id = 313; id <= 384; ++id)
            require(index.get(id) && *index.get(id) == std::to_string(id),
                "tail release preserves surviving pages and adjacent blocks");
        index.emplace(257, "revived");
        require(index.get(257) && *index.get(257) == "revived",
            "reuse page ID in released block");
    }
    {
        mdb_page_index index;
        for (uint64_t id = 321; id <= 384; ++id)
            index.emplace(id, std::to_string(id));
        const auto sparse_address = reinterpret_cast<uintptr_t>(index.get(321));
        index.promote_tail(384);
        const auto* tail_page = index.get(321);
        require(tail_page && *tail_page == "321" &&
            reinterpret_cast<uintptr_t>(tail_page) != sparse_address,
            "promote tail before building a dense prefix");
        for (uint64_t id = 1; id <= 32; ++id) index.emplace(id, "prefix");
        index.promote_prefix(32);
        require(index.size() == 96 && index.get(321) == tail_page &&
            index.get(32) && *index.get(32) == "prefix" && !index.get(320),
            "prefix growth retains a previously promoted tail");
    }
    {
        mdb_page_index index;
        std::vector<uint64_t> prefix(256);
        for (size_t i = 0; i < prefix.size(); ++i) prefix[i] = i + 1;
        index.prepare_empty(prefix.begin(), prefix.end(), [](uint64_t id) { return id; });
        for (uint64_t id : prefix) index.emplace(id, "prefix");
        index.emplace(258, "gap");
        for (uint64_t id = 321; id <= 384; ++id) index.emplace(id, std::to_string(id));
        const auto sparse_address = reinterpret_cast<uintptr_t>(index.get(321));
        index.promote_tail(384);
        require(index.size() == 321 && !index.get(257) &&
            index.get(258) && *index.get(258) == "gap" &&
            reinterpret_cast<uintptr_t>(index.get(321)) != sparse_address,
            "promote occupied tail block beyond a sparse gap");
        for (uint64_t id = 321; id <= 384; ++id)
            require(index.get(id) && *index.get(id) == std::to_string(id),
                "tail promotion beyond a gap retained page contents");
    }
    {
        mdb_page_index index;
        for (uint64_t id = 1; id <= 34; ++id)
            if (id != 2) index.emplace(id, std::to_string(id));
        const auto sparse_address = reinterpret_cast<uintptr_t>(index.get(3));
        index.promote_prefix(33);
        require(index.size() == 33 && !index.get(2) &&
            reinterpret_cast<uintptr_t>(index.get(3)) != sparse_address,
            "promote occupied prefix across an early hole");
        for (uint64_t id = 1; id <= 34; ++id)
            if (id != 2)
                require(index.get(id) && *index.get(id) == std::to_string(id),
                    "prefix promotion with holes retained page contents");
        index.emplace(2, "");
        require(index.size() == 34 && index.get(2) && index.get(2)->empty(),
            "reuse hole after prefix promotion");
    }
    {
        mdb_page_index index;
        index.emplace(2, "");
        const auto* second = index.get(2);
        require(second && second->empty(), "inline empty page is present");
        index.emplace(1, "first");
        index.emplace(8, "extension");
        const auto* eighth = index.get(8);
        require(eighth && *eighth == "extension" && index.get(2) == second,
            "page address survives extension allocation");
        index.emplace(UINT64_MAX, "sparse");
        require(index.size() == 4 && index.get(2) == second,
            "inline page address survives sparse insertion");
        index.erase(1);
        mdb_page_index target;
        target.swap(index);
        require(index.empty() && target.size() == 3 && !target.get(1) &&
            target.get(2) == second && second->empty() && target.get(8) == eighth,
            "inline erase and family swap retain presence");
        size_t visited = 0;
        require(target.visit([&](uint64_t id, const std::string& value) {
            ++visited;
            if (id == 8) return value == "extension";
            return id == 2 ? value.empty() : id == UINT64_MAX && value == "sparse";
        }) && visited == 3, "visit inline, extension and sparse pages");
        target.erase(8);
        require(target.size() == 2 && !target.get(8) && target.get(2) == second,
            "erase extension without moving the first block");
    }
    {
        mdb_page_index index;
        std::vector<uint64_t> ids(256);
        for (size_t i = 0; i < ids.size(); ++i) ids[i] = i + 1;
        index.prepare_empty(ids.begin(), ids.end(), [](uint64_t id) { return id; });
        std::string payload(29, 'p');
        payload.reserve(256);
        for (uint64_t id : ids) index.emplace(id, payload);
        index.emplace(257, payload);
        index.emplace(258, payload);
        const auto compact_capacity = std::string(payload).capacity();
        for (uint64_t id : {uint64_t(1), uint64_t(257), uint64_t(258)}) {
            const auto* stored = index.get(id);
            require(stored && *stored == payload && stored->capacity() == compact_capacity,
                "dense page retained extra string capacity");
        }
    }
    auto database = memory();
    const std::string owner = "dense";
    require(database->transact_page(owner, [&](page_transaction& tx) {
        put_root(tx, owner, "root");
        for (uint64_t id = 1; id <= 32; ++id)
            if (!tx.put(owner, id, std::to_string(id))) return false;
        return tx.put(owner, UINT64_MAX, "sparse");
    }) == batch_status::ok, "seed dense prefix and sparse tail");
    require(database->transact_page(owner, [&](page_transaction& tx) {
        return tx.erase(owner, 1) && tx.put(owner, 2, "") &&
            tx.put(owner, 33, "added") && tx.put(owner, UINT64_MAX, "tail");
    }) == batch_status::ok, "update dense prefix and sparse tail");
    read_page(database, owner, 1, false);
    require(read_page(database, owner, 2).empty() &&
        read_page(database, owner, 32) == "32" &&
        read_page(database, owner, 33) == "added" &&
        read_page(database, owner, UINT64_MAX) == "tail",
        "dense and sparse page values");
    require(database->transact_page(owner, [&](page_transaction& tx) {
        return tx.put(owner, 1, "revived");
    }) == batch_status::ok && read_page(database, owner, 1) == "revived",
        "reuse dense prefix hole");

    const std::string growing = "growing";
    for (uint64_t id = 1; id <= 530; ++id) {
        require(database->transact_page(growing, [&](page_transaction& tx) {
            if (id == 1) put_root(tx, growing, "root");
            return tx.put(growing, id, std::to_string(id)) &&
                (id == 1 || tx.put(growing, 1, std::to_string(id)));
        }) == batch_status::ok, "grow dense prefix incrementally");
    }
    require(read_page(database, growing, 1) == "530",
        "replacement published before prefix promotion");
    for (uint64_t id = 2; id <= 530; ++id)
        require(read_page(database, growing, id) == std::to_string(id),
            "prefix growth retained page");
    require(database->transact_page(growing, [&](page_transaction& tx) {
        for (uint64_t id = 321; id <= 384; ++id)
            if (!tx.erase(growing, id)) return false;
        return true;
    }) == batch_status::ok, "erase a middle MDB tail block");
    read_page(database, growing, 321, false);
    read_page(database, growing, 384, false);
    require(database->transact_page(growing, [&](page_transaction& tx) {
        return tx.put(growing, 384, "revived");
    }) == batch_status::ok && read_page(database, growing, 384) == "revived" &&
        read_page(database, growing, 320) == "320" &&
        read_page(database, growing, 385) == "385", "reuse the middle tail hole");
    require(database->transact_page(growing, [&](page_transaction& tx) {
        for (uint64_t id = 513; id <= 530; ++id)
            if (!tx.erase(growing, id)) return false;
        return true;
    }) == batch_status::ok, "erase last dense block");
    read_page(database, growing, 530, false);
    require(database->transact_page(growing, [&](page_transaction& tx) {
        return tx.put(growing, 514, "sparse");
    }) == batch_status::ok, "stage an out-of-order sparse page");
    require(database->transact_page(growing, [&](page_transaction& tx) {
        return tx.put(growing, 513, "first");
    }) == batch_status::ok && read_page(database, growing, 513) == "first" &&
        read_page(database, growing, 514) == "sparse" &&
        read_page(database, growing, 512) == "512", "retain the sparse page");

    // Leave a gap in the last dense block, then complete its sparse
    // successor in the same commit that replaces an existing page there.
    require(database->transact_page(growing, [&](page_transaction& tx) {
        for (uint64_t id = 449; id <= 480; ++id)
            if (!tx.erase(growing, id)) return false;
        for (uint64_t id = 515; id < 576; ++id)
            if (!tx.put(growing, id, std::to_string(id))) return false;
        return true;
    }) == batch_status::ok, "populate sparse successor");
    require(database->transact_page(growing, [&](page_transaction& tx) {
        return tx.put(growing, 514, "replaced") &&
            tx.erase(growing, 550) && tx.put(growing, 576, "last");
    }) == batch_status::ok, "publish sparse changes around promotion");
    read_page(database, growing, 550, false);
    require(read_page(database, growing, 513) == "first" &&
        read_page(database, growing, 514) == "replaced" &&
        read_page(database, growing, 575) == "575" &&
        read_page(database, growing, 576) == "last",
        "promotion retains values and applies pending deletion");
}

static void test_multi_owner_addresses() {
    for (bool replace : {false, true}) {
        auto engine = std::make_shared<ordered_pages_db>();
        shared_db database = engine;
        require(database->open(""), "address engine open");
        const std::vector<std::string> names = {
            std::string(256, 'z'), std::string("a\0x", 3), ""};
        auto keys = names;
        keys.push_back(names[1]);
        require(database->transact_pages(keys, [&](page_transaction& tx) {
            if (replace)
                require(tx.put(names[0], UINT64_MAX, "first page"),
                    "stage first page at a distinct address");
            for (const auto& name : names) {
                put_root(tx, name, "root");
                for (uint64_t id : {3U, 1U, 2U})
                    require(tx.put(name, id, name + std::to_string(id)),
                        "interleaved owner stage");
                auto alias = name;
                require(tx.put(alias, 4, std::move(alias)),
                    "multi-owner moved alias");
            }
            if (replace) {
                require(tx.reset_pages(names[1]), "middle owner reset");
                for (uint64_t id : {3U, 1U, 2U})
                    require(tx.put(names[1], id, std::to_string(id)),
                        "replacement prefix stage");
                require(tx.replace_pages(names[1], 2), "middle owner seal");
                std::string out;
                require(tx.lookup(names[0], UINT64_MAX, out) ==
                    lookup_status::found && out == "first page",
                    "sealing another owner hid the first staged page");
            }
            for (const auto& name : names) {
                std::string out;
                require(tx.lookup(name, 2, out) == lookup_status::found &&
                    out == (replace && name == names[1] ? "2" : name + "2"),
                    "cross-owner read own write");
            }
            return true;
        }) == batch_status::ok, "multi-owner address commit");
        require(engine->observed == (replace ? 11U : 12U),
            "reset/seal changed another owner");
        for (const auto& name : names) {
            const bool sealed = replace && name == names[1];
            require(read_page(database, name, 1) == (sealed ? "1" : name +
                "1"), "committed owner address");
            if (sealed) read_page(database, name, 3, false);
            else require(read_page(database, name, 4) == name,
                "committed aliased owner bytes");
        }
    }
}

static void test_read_only_staging() {
    auto database = memory();
    seed(database, "owner", 2);
    for (unsigned count : {1U, 2U}) {
        require(database->transact_page_reads({"owner"},
            [&](page_transaction& tx) {
                for (unsigned id = 1; id <= count; ++id)
                    require(tx.put("owner", id, std::string("changed")),
                        "read-only stage fixture");
                return true;
            }) == batch_status::aborted, "read-only staging committed");
        require(read_page(database, "owner", 1) == "page:1" &&
            read_page(database, "owner", 2) == "page:2",
            "read-only staging changed pages");
    }
}

static void test_copy_source_staging() {
    for (bool same : {false, true}) {
        for (bool source_first : {false, true}) {
            auto source = memory();
            auto target = same ? source : memory();
            seed(source, "source", 1); seed(target, "target", 1);
            const auto result = source->copy_pages_to("source", *target,
                "target", [&](page_transaction& from, page_transaction& to) {
                    put_root(to, "target", "changed");
                    if (source_first)
                        require(from.put("source", 1, "illegal"),
                            "source first fixture");
                    require(to.put("target", 1, "changed"),
                        "target copy fixture");
                    if (!source_first)
                        require(from.put("source", 1, "illegal"),
                            "source second fixture");
                    return true;
                });
            require(result == batch_status::aborted &&
                read_page(source, "source", 1) == "page:1" &&
                read_page(target, "target", 1) == "page:1",
                "COPY allowed source page staging");
        }
    }
}

static void test_validation_and_abort() {
    auto database = memory();
    seed(database, "owner", 1);
    require(database->transact_pages({"owner"}, [&](page_transaction& view) {
        put_root(view, "owner", "changed");
        require(view.put("owner", 1, "changed"), "abort stage");
        return false;
    }) == batch_status::aborted, "callback abort ignored");
    require(read_page(database, "owner", 1) == "page:1", "abort changed page");
    for (unsigned mode = 0; mode < 5; ++mode) {
        require(database->transact_pages({"owner"},
            [&](page_transaction& view) {
                put_root(view, "owner", "changed");
                if (mode == 0) {
                    require(!view.put("outside", 1, "x"), "unlocked owner");
                } else if (mode == 1) {
                    require(!view.erase("owner", 0), "zero page id");
                } else if (mode == 2) {
                    require(!view.put("owner", 1, std::string(16385, 'x')),
                        "oversize page");
                } else if (mode == 3) {
                    put_root(view, "owner", std::string(16385, 'x'));
                } else {
                    view.roots().find("owner")->action = action::erase;
                    require(view.put("owner", 1, "orphan"), "orphan stage");
                }
                return true;
            }) == batch_status::aborted, "invalid transaction committed");
        require(read_page(database, "owner", 1) == "page:1",
            "validation failure changed page");
    }
    require(database->transact_pages({"missing"}, [&](page_transaction& view) {
        return view.put("missing", 1, "orphan");
    }) == batch_status::aborted, "page without root accepted");
    bool threw = false;
    try {
        database->transact_pages({"owner"}, [&](page_transaction& view) {
            view.put("owner", 1, "exception");
            throw std::runtime_error("callback");
            return true;
        });
    } catch (const std::runtime_error&) {
        threw = true;
    }
    require(threw && read_page(database, "owner", 1) == "page:1",
        "throwing callback leaked changes/locks");
    require(database->transact_pages({}, [](page_transaction&) {
        return true;
    }) == batch_status::aborted, "empty owner set accepted");
}

static void test_cleanup() {
    for (unsigned mode = 0; mode < 7; ++mode) {
        auto database = memory();
        seed(database, "owner", 3);
        bool ok = false;
        if (mode == 0) {
            ok = database->del("owner");
        } else if (mode == 1) {
            ok = database->set("owner", "overwrite");
        } else if (mode == 2) {
            ok = database->replica_set("owner", "overwrite");
        } else if (mode == 3) {
            ok = database->replica_erase("owner");
        } else if (mode == 4) {
            ok = database->transact({"owner", "other"},
                [](db::batch_view roots) {
                    for (auto& root : roots) {
                        root.action = action::write;
                        root.value = "overwrite";
                    }
                    return true;
                }) == batch_status::ok;
        } else if (mode == 5) {
            ok = database->update("owner", [](bool, std::string&) {
                return action::erase;
            });
        } else {
            ok = database->transact_pages({"owner"},
                [](page_transaction& view) {
                    view.roots().find("owner")->action = action::erase;
                    std::string out = "sentinel";
                    require(view.lookup("owner", 1, out) ==
                        lookup_status::missing && out == "sentinel",
                        "read after staged root deletion");
                    return true;
                }) == batch_status::ok;
        }
        require(ok, "root cleanup operation failed");
        for (uint64_t id = 1; id <= 3; ++id) {
            read_page(database, "owner", id, false);
        }
        seed(database, "owner", 1);
        require(read_page(database, "owner", 1) == "page:1",
            "recreated owner inherited stale pages");
        read_page(database, "owner", 2, false);
    }
}

class failing_memory : public mdb {
public:
    failing_memory() : mdb(2) {}
    bool fail_root = false;
    bool fail_page = false;
    bool oversized_page = false;
    bool append_page = false;
protected:
    lookup_status dblookup(const std::string& key,
        std::string& out) override {
        return fail_root ? lookup_status::error : db::dblookup(key, out);
    }
    lookup_status dblookup_page(const std::string& owner, uint64_t id,
        std::string& out) override {
        if (fail_page || oversized_page) {
            out = std::string(oversized_page ? 16385 : 1, 'x');
            return oversized_page ? lookup_status::found : lookup_status::error;
        }
        if (append_page) {
            std::string bytes;
            const auto status = mdb::dblookup_page(owner, id, bytes);
            if (status == lookup_status::found) out.append(bytes);
            return status;
        }
        return mdb::dblookup_page(owner, id, out);
    }
};

class changing_key_memory : public mdb {
public:
    changing_key_memory() : mdb(2) {}
    std::string* input = nullptr;
protected:
    lookup_status dblookup(const std::string& key,
        std::string& value) override {
        if (input) { input->assign(256, 'z'); input = nullptr; }
        return db::dblookup(key, value);
    }
};

static void test_single_key_hook_snapshot() {
    auto database = std::make_shared<changing_key_memory>();
    require(database->open(""), "key hook memory open");
    const std::string expected(128, 'k');
    std::string key = expected;
    require(database->set(expected, "original"), "key hook seed");
    database->input = &key;
    require(database->transact_page(key, [&](page_transaction& tx) {
        const auto& root = *tx.roots().begin();
        return tx.roots().size() == 1 && root.key == expected &&
            root.found && root.value == "original";
    }) == batch_status::ok && key == std::string(256, 'z'),
        "single key changed across backend hook/lock boundary");
    key = expected;
    database->input = &key;
    require(database->replace_page_root(key, [&](db::batch_entry& entry) {
        require(entry.key == expected && entry.found && entry.value.empty(),
            "root replacement did not preserve key and existence");
        entry.value = "replacement";
        entry.action = db::update_action::write;
        entry.clear_pages = true;
        return db::page_root_result::ready;
    }) == batch_status::ok && key == std::string(256, 'z'),
        "root replacement failed after backend hook changed caller key");
    std::string value;
    require(database->get(expected, value) && value == "replacement",
        "root replacement wrote a different key");
}

static void test_single_key_entry() {
    const std::string original("owner\0binary", 12);
    auto database = memory();
    seed(database, original, 2);
    std::string key = original;
    require(database->transact_page(key, [&](page_transaction& tx) {
        key = "changed caller input";
        require(tx.roots().size() == 1 &&
            tx.roots().begin()->key == original, "single root snapshot");
        put_root(tx, original, "new-root");
        return tx.erase(original, 1) && tx.put(original, 2, "new-page");
    }) == batch_status::ok, "single key snapshot commit");
    require(read_page(database, original, 2) == "new-page",
        "single key page update");
    read_page(database, original, 1, false);
    require(database->transact_page("", [&](page_transaction& tx) {
        put_root(tx, "", "empty key");
        return tx.put("", 1, "empty owner page");
    }) == batch_status::ok && read_page(database, "", 1) ==
        "empty owner page", "single empty owner");
    bool success = false;
    unsigned failures = 0;
    const std::string owner(128, 'k'), replacement(512, 'v');
    for (long budget = 0; budget < 128; ++budget) {
        database = memory(); seed(database, owner, 2);
        const db::page_callback callback = [&](page_transaction& tx) {
            put_root(tx, owner, replacement);
            return tx.put(owner, 1, replacement) && tx.erase(owner, 2);
        };
        allocation_budget = budget;
        const auto status = database->transact_page(owner, callback);
        allocation_budget = -1;
        if (status == batch_status::ok) { success = true; break; }
        ++failures;
        std::string root;
        require(status == batch_status::error &&
            database->get(owner, root) && root == "root" &&
            read_page(database, owner, 1) == "page:1" &&
            read_page(database, owner, 2) == "page:2",
            "single allocation failure changed root/pages");
    }
    require(success && failures > 0, "single allocation failure sweep");
}

static void test_unsupported_and_errors() {
    auto unsupported = db::create_mdb_avl();
    require(unsupported->open(""), "legacy engine open");
    require(unsupported->set("existing", "value"), "legacy engine setup");
    bool called = false;
    require(unsupported->transact_pages({"owner"}, [&](page_transaction&) {
        called = true;
        return true;
    }) == batch_status::unsupported && !called,
        "unsupported engine entered callback");
    require(unsupported->transact_page("owner", [&](page_transaction&) {
        called = true; return true;
    }) == batch_status::unsupported && !called,
        "unsupported single-key engine entered callback");
    std::string legacy;
    require(unsupported->get("existing", legacy) && legacy == "value",
        "unsupported page transaction changed legacy engine");
    auto failing = std::make_shared<failing_memory>();
    require(failing->open(""), "failing memory open");
    seed(failing, "owner", 1);
    failing->fail_root = true;
    require(failing->transact_pages({"owner"}, [&](page_transaction&) {
        called = true;
        return true;
    }) == batch_status::error && !called,
        "failed root lookup entered callback");
    require(failing->transact_page("owner", [&](page_transaction&) {
        called = true; return true;
    }) == batch_status::error && !called,
        "single-key root error entered callback");
    require(failing->replace_page_root("owner", [&](db::batch_entry&) {
        called = true; return db::page_root_result::rejected;
    }) == batch_status::error && !called,
        "root replacement bypassed backend lookup error");
    failing->fail_root = false;
    failing->fail_page = true;
    require(failing->transact_pages({"owner"}, [&](page_transaction& view) {
        put_root(view, "owner", "changed");
        std::string out = "sentinel";
        require(view.lookup("owner", 1, out) == lookup_status::error &&
            out == "sentinel", "failed page lookup changed output");
        return true;
    }) == batch_status::error, "failed page lookup committed");
    failing->fail_page = false;
    failing->oversized_page = true;
    require(failing->transact_pages({"owner"}, [&](page_transaction& view) {
        std::string out = "sentinel";
        require(view.lookup("owner", 1, out) == lookup_status::error &&
            out == "sentinel", "oversize stored page accepted");
        return true;
    }) == batch_status::error, "oversize page not treated as storage error");
    failing->oversized_page = false;
    require(read_page(failing, "owner", 1) == "page:1", "error changed data");
    failing->append_page = true;
    require(failing->transact_pages({"owner"}, [&](page_transaction& view) {
        std::string scratch = "discarded contents";
        for (unsigned i = 0; i < 3; ++i)
            require(view.lookup_scratch("owner", 1, scratch) ==
                lookup_status::found && scratch == "page:1",
                "scratch backend did not receive an empty buffer");
        return true;
    }) == batch_status::ok, "append-style backend scratch read");
    failing->append_page = false;
    for (bool oversized : {false, true}) {
        failing->fail_page = !oversized;
        failing->oversized_page = oversized;
        require(failing->transact_pages({"owner"},
            [&](page_transaction& view) {
                put_root(view, "owner", "changed");
                std::string scratch = "old scratch";
                require(view.lookup_scratch("owner", 1, scratch) ==
                    lookup_status::error, "scratch storage error status");
                return true;
            }) == batch_status::error, "ignored scratch error committed");
        failing->fail_page = failing->oversized_page = false;
        std::string root;
        require(failing->get("owner", root) && root == "root" &&
            read_page(failing, "owner", 1) == "page:1",
            "scratch storage error changed root/pages");
    }
    db_watchers watchers;
    auto observed = db::create_mdb(2);
    require(observed->open("", &watchers), "observed memory open");
    require(observed->transact_pages({"owner"}, [&](page_transaction& view) {
        put_root(view, "owner", "root");
        return view.put("owner", 1, "page");
    }) == batch_status::unsupported, "pages published as plain root event");
    std::string out;
    require(!observed->get("owner", out), "observer rejection wrote root");
    require(observed->transact_pages({"owner"}, [](page_transaction&) {
        return true;
    }) == batch_status::ok, "observer prevented read-only callback");
}

static void test_allocation_failures() {
    for (bool commit_only : {false, true}) {
        bool succeeded = false;
        size_t failures = 0;
        for (long budget = 0; budget < 512; ++budget) {
            auto database = memory();
            seed(database, "old", 3);
            seed(database, "erased", 2);
            std::vector<std::string> names = {"old", "new", "erased"};
            const db::page_callback callback = [&](page_transaction& view) {
                    put_root(view, "old", "new-root");
                    put_root(view, "new", "new-root");
                    view.roots().find("erased")->action = action::erase;
                    view.put("old", 1, std::string(1000, 'r'));
                    view.put("old", 3, std::string(1000, 's'));
                    view.erase("old", 2);
                    view.put("old", 4, std::string(1000, 'a'));
                    for (uint64_t id = 1; id <= 8; ++id) {
                        view.put("new", id, std::string(1000, 'n'));
                    }
                    if (commit_only) {
                        allocation_budget = budget;
                    }
                    return true;
                };
            if (!commit_only) {
                allocation_budget = budget;
            }
            const auto result = database->transact_pages(std::move(names),
                callback);
            allocation_budget = -1;
            if (result == batch_status::error) {
                ++failures;
                std::string root;
                require(database->get("old", root) && root == "root" &&
                    !database->get("new", root) &&
                    database->get("erased", root) && root == "root",
                    "allocation failure exposed root changes");
                for (uint64_t id = 1; id <= 3; ++id) {
                    require(read_page(database, "old", id) ==
                        "page:" + std::to_string(id),
                        "allocation failure exposed page changes");
                }
                read_page(database, "old", 4, false);
                for (uint64_t id = 1; id <= 8; ++id) {
                    read_page(database, "new", id, false);
                }
                require(read_page(database, "erased", 1) == "page:1",
                    "allocation failure reclaimed old pages");
            } else {
                require(result == batch_status::ok, "allocation status");
                require(read_page(database, "old", 1) ==
                    std::string(1000, 'r'), "replacement omitted");
                require(read_page(database, "old", 3) ==
                    std::string(1000, 's'), "multipage replacement omitted");
                read_page(database, "old", 2, false);
                read_page(database, "erased", 1, false);
                require(read_page(database, "new", 8) ==
                    std::string(1000, 'n'), "insertion omitted");
                succeeded = true;
                break;
            }
        }
        require(succeeded && failures > 0, "allocation sweep incomplete");
    }
}

static bool increment_pair(page_transaction& view) {
    std::string left, right;
    require(view.lookup("left", 1, left) == lookup_status::found &&
        view.lookup("right", 1, right) == lookup_status::found,
        "concurrent page lookup");
    require(left == right && left == view.roots().find("left")->value &&
        left == view.roots().find("right")->value,
        "torn root/page/multi-key snapshot");
    acl::fiber::yield();
    const auto next = std::to_string(std::stoll(left) + 1);
    put_root(view, "left", next);
    put_root(view, "right", next);
    return view.put("left", 1, next) && view.put("right", 1, next);
}

static void test_concurrent_atomicity() {
    auto database = memory();
    seed(database, "left", 1);
    seed(database, "right", 1);
    require(database->transact_pages({"left", "right"},
        [&](page_transaction& view) {
            put_root(view, "left", "0");
            put_root(view, "right", "0");
            return view.put("left", 1, "0") && view.put("right", 1, "0");
        }) == batch_status::ok, "concurrency setup");
    std::vector<std::thread> workers;
    std::atomic<unsigned> ready{0};
    for (unsigned worker = 0; worker < 4; ++worker) {
        workers.emplace_back([&, worker] {
            ++ready;
            while (ready.load() != 4) {
                std::this_thread::yield();
            }
            for (unsigned fiber = 0; fiber < 4; ++fiber) go[&, worker] {
                for (unsigned i = 0; i < 50; ++i) {
                    const std::vector<std::string> keys = worker % 2 ?
                        std::vector<std::string>{"left", "right", "left"} :
                        std::vector<std::string>{"right", "left"};
                    require(database->transact_pages(keys,
                        increment_pair) == batch_status::ok,
                        "concurrent transaction failed");
                }
            };
            acl::fiber::schedule();
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
    require(read_page(database, "left", 1) == "800", "lost concurrent write");
}

static void test_raw_writer_isolation() {
    for (bool erase : {false, true}) {
        auto database = memory();
        seed(database, "owner", 1);
        bool entered = false;
        go[&] {
            require(database->transact_pages({"owner"},
                [&](page_transaction& view) {
                    entered = true;
                    acl::fiber::yield();
                    put_root(view, "owner", "paged-root");
                    return view.put("owner", 1, "paged-value");
                }) == batch_status::ok, "yielding page update");
        };
        go[&] {
            while (!entered) {
                acl::fiber::yield();
            }
            require(erase ? database->replica_erase("owner") :
                database->set("owner", "raw-after"),
                "competing raw mutation");
        };
        acl::fiber::schedule();
        std::string out;
        const bool found = database->get("owner", out);
        require(erase ? !found : found && out == "raw-after",
            "raw writer bypassed page transaction key lock");
        read_page(database, "owner", 1, false);
    }
}

static void test_fragmented_list_storage() {
    auto database = memory();
    const std::string owner("list\0owner", 10);
    const std::string member(1024 * 1024, '\x80');
    const auto fragment_bytes = list_page_codec::max_fragment_bytes;
    const auto pages = (member.size() + fragment_bytes - 1) / fragment_bytes;
    require(database->transact_pages({owner}, [&](page_transaction& view) {
        list_root root;
        root.elements = 1;
        root.head = 1;
        root.tail = pages;
        root.expires_at_ms = 4102444800123;
        std::string bytes;
        require(list_page_codec::encode_root(root, bytes) ==
            list_page_status::ok, "stored List root encode");
        put_root(view, owner, bytes);
        size_t offset = 0;
        for (size_t i = 1; i <= pages; ++i) {
            list_page page;
            page.id = i;
            page.previous = i == 1 ? 0 : i - 1;
            page.next = i == pages ? 0 : i + 1;
            list_fragment fragment;
            fragment.total_bytes = member.size();
            fragment.offset = offset;
            fragment.bytes = member.substr(offset, fragment_bytes);
            offset += fragment.bytes.size();
            page.fragments.push_back(std::move(fragment));
            require(list_page_codec::encode_page(page, bytes) ==
                list_page_status::ok && view.put(owner, i, bytes),
                "bounded List page stage");
        }
        return true;
    }) == batch_status::ok, "fragmented List storage commit");
    std::string root_bytes;
    require(database->get(owner, root_bytes) && root_bytes.size() == 40,
        "List root contains serialized member");
    size_t recovered = 0;
    require(database->transact_pages({owner}, [&](page_transaction& view) {
        list_root root;
        require(list_page_codec::decode_root(
            view.roots().find(owner)->value, root) == list_page_status::ok &&
            root.elements == 1 && root.tail == pages &&
            root.expires_at_ms == 4102444800123, "stored root roundtrip");
        for (uint64_t id = root.head; id != 0;) {
            std::string bytes;
            list_page page;
            require(view.lookup(owner, id, bytes) == lookup_status::found &&
                bytes.size() <= 16384 && list_page_codec::decode_page(bytes,
                    page) == list_page_status::ok, "stored page roundtrip");
            const auto& fragment = page.fragments.front();
            require(fragment.offset == recovered && fragment.bytes ==
                member.substr(recovered, fragment.bytes.size()),
                "stored member fragment mismatch");
            recovered += fragment.bytes.size();
            id = page.next;
        }
        return true;
    }) == batch_status::ok && recovered == member.size(),
        "fragmented List storage incomplete");
}

static size_t measure_update(size_t pages) {
    auto database = memory();
    seed(database, "owner", pages);
    allocated_bytes = 0;
    measure_allocations = true;
    const auto result = database->transact_pages({"owner"},
        [&](page_transaction& view) {
            put_root(view, "owner", "new-root");
            return view.put("owner", 1, std::string(100, 'x'));
        });
    measure_allocations = false;
    require(result == batch_status::ok, "measured update failed");
    return allocated_bytes;
}

static void test_root_workspace_isolation() {
    std::vector<std::thread> workers;
    for (unsigned worker = 0; worker < 4; ++worker) workers.emplace_back([] {
        auto outer = memory(), other = memory();
        seed(outer, "owner", 1);
        bool entered = false, finished = false;
        go[&] {
            require(outer->transact_page("owner", [&](page_transaction& tx) {
                auto& root = *tx.roots().begin();
                root.value = "outer"; root.action = action::write;
                require(other->transact_page("nested",
                    [&](page_transaction& nested) {
                    require(nested.roots().size() == 1,
                        "nested transaction shared active roots");
                    put_root(nested, "nested", "inner"); return true;
                }) == batch_status::ok && root.value == "outer",
                    "nested transaction changed outer root");
                entered = true;
                while (!finished) acl::fiber::yield();
                require(root.key == "owner" && root.value == "outer",
                    "yielded transaction root changed");
                return tx.put("owner", 1, "outer-page");
            }) == batch_status::ok, "yielded root workspace commit");
        };
        go[&] {
            while (!entered) acl::fiber::yield();
            require(other->transact_page("other", [&](page_transaction& tx) {
                put_root(tx, "other", "other-value");
                return tx.put("other", 1, "other-page");
            }) == batch_status::ok, "concurrent fallback workspace");
            finished = true;
        };
        acl::fiber::schedule();
        require(read_page(outer, "owner", 1) == "outer-page" &&
            read_page(other, "other", 1) == "other-page",
            "thread-local root workspace lost committed pages");
    });
    for (auto& worker : workers) worker.join();
}

class held_page_memory : public mdb {
public:
    held_page_memory() : mdb(2) {}
    std::function<void(const std::vector<page_change>&)> before;
    bool reject = false;
protected:
    batch_status dbapply_pages(const std::vector<batch_entry>& roots,
        const std::vector<page_change>& pages) override {
        if (before) before(pages);
        return reject ? batch_status::error : mdb::dbapply_pages(roots, pages);
    }
};
class page_lease_observer : public db_watcher {
public:
    std::function<void(const std::vector<page_change>&)> check;
    bool supports_pages() const override { return true; }
    bool on_get(const std::string&, const std::string&, bool) override {
        return true;
    }
    bool on_set(const std::string&, const std::string&, bool) override {
        return true;
    }
    bool on_del(const std::string&, bool) override { return true; }
    bool on_pages_database(unsigned, const std::vector<db::batch_entry>&,
        const std::vector<page_change>& pages) override {
        if (check) check(pages);
        return true;
    }
};
static void test_released_page_lifetime() {
    std::vector<std::thread> workers;
    for (unsigned worker = 0; worker < 4; ++worker) workers.emplace_back([] {
        { detail::node_write_workspace warm; warm.buffer().assign(128, 'w'); }
        const std::string owner(256, 'o');
        for (bool reject : {false, true}) {
            page_lease_observer observer;
            db_watchers watchers; watchers.add_watcher(&observer);
            auto outer = std::make_shared<held_page_memory>();
            auto peer = memory();
            require(outer->open("", &watchers), "page lease open");
            seed(outer, owner, 1);
            const std::string payload(128, 'o');
            bool entered = false, finished = false;
            unsigned observed = 0;
            auto nested = [&](const std::string& key) {
                require(peer->transact_page(key, [&](page_transaction& tx) {
                    put_root(tx, key, "inner");
                    detail::node_write_workspace bytes;
                    bytes.buffer().assign(512, 'i');
                    return tx.put(key, 2, std::move(bytes.buffer())) &&
                        tx.put(key, 3, "inner page");
                }) == batch_status::ok, "nested page release");
            };
            auto intact = [&](const std::vector<page_change>& pages) {
                require(pages.size() == 2 && pages[0].owner == owner &&
                    pages[0].id == 1 && pages[0].value == payload &&
                    pages[0].action == action::write &&
                    pages[1].owner == owner && pages[1].id == 2 &&
                    pages[1].value == "outer page",
                    "outer released pages changed across a callback");
            };
            outer->before = [&](const std::vector<page_change>& pages) {
                intact(pages); nested("nested");
                entered = true;
                while (!finished) acl::fiber::yield();
                intact(pages);
            };
            observer.check = [&](const std::vector<page_change>& pages) {
                ++observed; intact(pages); nested("observer"); intact(pages);
            };
            outer->reject = reject;
            go[&] {
                require(outer->transact_page(owner,
                    [&](page_transaction& tx) {
                    put_root(tx, owner, "new-root");
                    detail::node_write_workspace bytes;
                    bytes.buffer() = payload;
                    return tx.put(owner, 1, std::move(bytes.buffer())) &&
                        tx.put(owner, 2, "outer page");
                }) == (reject ? batch_status::error : batch_status::ok),
                    "held page result");
            };
            go[&] {
                while (!entered) acl::fiber::yield();
                nested("other"); finished = true;
            };
            acl::fiber::schedule();
            outer->before = nullptr; observer.check = nullptr;
            if (reject) read_page(outer, owner, 2, false);
            else require(read_page(outer, owner, 2) == "outer page",
                "second staged page was not committed");
            require(observed == (reject ? 0U : 1U) &&
                read_page(outer, owner, 1) ==
                    (reject ? "page:1" : payload) &&
                read_page(peer, "other", 2) == std::string(512, 'i'),
                "released page commit/rollback ownership");
        }
    });
    for (auto& worker : workers) worker.join();
}
static void test_page_workspace_capacity() {
    auto database = memory();
    seed(database, "bulk", 160);
    require(detail::root_workspace::retained_page_capacity() <= 128,
        "workspace retained oversized page-change storage");
    bool succeeded = false;
    unsigned failures = 0;
    for (long budget = 0; budget < 64; ++budget) {
        auto value = memory(); seed(value, "owner", 1);
        const auto result = value->transact_page("owner",
            [&](page_transaction& tx) {
            put_root(tx, "owner", "new-root");
            if (!tx.put("owner", 2, std::string(512, 'n'))) return false;
            allocation_budget = budget;
            return true;
        });
        allocation_budget = -1;
        std::string root;
        if (result == batch_status::ok) {
            require(value->get("owner", root) && root == "new-root" &&
                read_page(value, "owner", 2) == std::string(512, 'n'),
                "cached page release lost committed bytes");
            succeeded = true; break;
        }
        require(result == batch_status::error &&
            value->get("owner", root) && root == "root" &&
            read_page(value, "owner", 1) == "page:1",
            "failed cached page release changed committed data");
        read_page(value, "owner", 2, false);
        ++failures;
    }
    require(succeeded && failures &&
        detail::root_workspace::retained_page_capacity() <= 128,
        "cached page release allocation sweep incomplete");
}
static void test_root_workspace_reset() {
    auto database = memory();
    require(database->set("huge", std::string(65536, 'x')),
        "workspace large root seed");
    require(database->transact_page("huge", [&](page_transaction& tx) {
        return tx.roots().begin()->value.size() == 65536;
    }) == batch_status::ok &&
        detail::root_workspace::retained_value_capacity() <=
            page_transaction::max_value_bytes,
        "workspace retained oversized root buffer");
    for (unsigned i = 0; i < 8; ++i) {
        require(database->transact_page("missing", [&](page_transaction& tx) {
            auto& root = *tx.roots().begin();
            require(!root.found && root.value.empty() &&
                root.action == action::keep && !root.clear_pages &&
                !root.maintenance_only && !root.force_watch_invalidation,
                "workspace reused stale root state");
            root.value = "abort"; root.action = action::write;
            root.clear_pages = root.maintenance_only = true;
            root.force_watch_invalidation = true;
            return false;
        }) == batch_status::aborted, "workspace callback abort");
    }
}

static void test_cached_node_lifetimes() {
    auto outer = memory(), peer = memory();
    bool entered = false, finished = false;
    go[&] {
        require(outer->transact_pages({"live"}, [&](page_transaction& tx) {
            put_root(tx, "live", "root");
            for (uint64_t id = 1; id <= 96; ++id)
                require(tx.put("live", id, "outer:" + std::to_string(id)),
                    "live node stage");
            entered = true;
            while (!finished) acl::fiber::yield();
            for (uint64_t id = 1; id <= 96; ++id) {
                std::string out;
                require(tx.lookup("live", id, out) == lookup_status::found &&
                    out == "outer:" + std::to_string(id),
                    "nested actor reused a live staged node");
            }
            return true;
        }) == batch_status::ok, "live staged commit");
    };
    go[&] {
        while (!entered) acl::fiber::yield();
        for (unsigned round = 0; round < 3; ++round)
            require(peer->transact_pages({"peer"}, [&](page_transaction& tx) {
                put_root(tx, "peer", "root");
                for (uint64_t id = 1; id <= 96; ++id)
                    if (!tx.put("peer", id, std::string(128, 'p')))
                        return false;
                return true;
            }) == batch_status::ok, "nested node recycling");
        finished = true;
    };
    acl::fiber::schedule();
    require(read_page(outer, "live", 96) == "outer:96", "live node bytes");
    using value = std::pair<const uint64_t, std::string>;
    using tree = std::map<uint64_t, std::string, std::less<uint64_t>,
        detail::page_node_allocator<value>>;
    tree original;
    for (uint64_t id = 0; id < 128; ++id)
        original.emplace(id, std::string(128, char(id)));
    auto copy = std::make_shared<tree>(original);
    original.clear();
    std::thread worker([copy] {
        for (uint64_t id = 0; id < 128; ++id)
            require(copy->at(id) == std::string(128, char(id)),
                "node reuse changed a live copy");
        copy->clear(); // Release original-worker allocations on this worker.
        for (uint64_t id = 0; id < 128; ++id)
            copy->emplace(id, std::string(128, 'w'));
    });
    worker.join();
    for (uint64_t id = 0; id < 128; ++id)
        require(copy->at(id) == std::string(128, 'w'), "cross-worker node");
    std::thread late_owner([] {
        // Register the container destructor before the pool's cleanup.
        // Its nodes will be released after caching has been shut down.
        static thread_local std::unique_ptr<tree> owner;
        owner.reset(new tree);
        for (uint64_t id = 0; id < 128; ++id)
            owner->emplace(id, std::string(128, 't'));
    });
    late_owner.join();
}

int main() {
    acl::acl_cpp_init();
    // Match server startup: initialize ACL's process-wide scheduler keys before
    // threads concurrently use fiber mutexes. Older installed ACL libraries
    // query an uninitialized key from acl_fiber_running().
    go[] {};
    acl::fiber::schedule();
    test_root_workspace_isolation();
    test_released_page_lifetime();
    test_page_workspace_capacity();
    test_root_workspace_reset();
    test_cached_node_lifetimes();
    test_single_key_hook_snapshot();
    test_single_key_entry();
    test_scratch_reads();
    test_namespace_and_ownership();
    test_moved_pages();
    test_single_owner_staging_order();
    test_dense_page_prefix();
    test_multi_owner_addresses();
    test_read_only_staging();
    test_copy_source_staging();
    test_validation_and_abort();
    test_cleanup();
    test_unsupported_and_errors();
    test_allocation_failures();
    test_concurrent_atomicity();
    test_raw_writer_isolation();
    test_fragmented_list_storage();
    const auto small = measure_update(10);
    const auto large = measure_update(5000);
    require(large <= small + 1024 && large < 32768,
        "one-page update copied whole page index");
    std::cout << "Atomic page storage tests passed; one-page allocations "
        << small << "/" << large << " bytes for 10/5000 stored pages\n";
}
