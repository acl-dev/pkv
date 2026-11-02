#include "stdafx.h"
#include "db/page_workspace.h"
#include "db/mdb/mdb.h"
#include "db/db_watcher.h"
#include "coder/string_page.h"
#include "coder/record_codec.h"
#include "dao/paged_string.h"
#include "dao/paged_list.h"
#include "dao/paged_hash.h"
#include "dao/paged_set.h"
#include "dao/paged_zset.h"
#include <cstdlib>
#include <iostream>
#include <new>
#include <set>
#include <random>

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
using action = db::update_action;
using status = db::batch_status;
static void require(bool ok, const char* text) {
    if (!ok) { std::cerr << text << '\n'; std::exit(1); }
}
static std::string encoded(const std::string& value) {
    string_root root; root.length = value.size(); root.inline_value = value;
    std::string out;
    require(string_page_codec::encode_root(root, out) ==
        string_page_status::ok, "root encode");
    return out;
}
static std::string read(db& database, const std::string& key) {
    std::string out;
    require(database.get(key, out), "missing root"); return out;
}
static bool page(db& database, const std::string& key, uint64_t id,
    std::string& out) {
    bool found = false;
    require(database.transact_pages({key}, [&](page_transaction& tx) {
        const auto result = tx.lookup(key, id, out);
        found = result == db::lookup_status::found;
        return result != db::lookup_status::error;
    }) == status::ok, "page read error");
    return found;
}
static status write(db& database, const std::string& key,
    const std::string& value, uint64_t id = 0, bool reset = false) {
    string_root metadata;
    metadata.length = value.size(); metadata.inline_value = value;
    std::string bytes;
    if (string_page_codec::encode_root(metadata, bytes) !=
        string_page_status::ok) return status::error;
    return database.transact_pages({key}, [&](page_transaction& tx) {
        auto& root = *tx.roots().find(key);
        root.action = action::write; root.value = bytes;
        root.clear_pages = reset;
        return !id || tx.put(key, id, value);
    });
}
static watch_token watch(shared_db database, const std::string& key) {
    std::vector<watch_token> tokens;
    require(database->watch({key}, tokens) == status::ok, "WATCH");
    return tokens.front();
}
struct observer : db_watcher {
    size_t calls = 0;
    bool throwing = false, page_capable = true;
    bool supports_pages() const override { return page_capable; }
    bool on_pages_database(unsigned, const std::vector<db::batch_entry>&,
        const std::vector<page_change>&) override {
        ++calls; if (throwing) throw std::bad_alloc(); return true;
    }
    bool on_set(const std::string&, const std::string&, bool) override {
        return true;
    }
    bool on_get(const std::string&, const std::string&, bool) override {
        return true;
    }
    bool on_del(const std::string&, bool) override { return true; }
};
struct counted_db : mdb {
    size_t commits = 0, page_reads = 0;
    bool reject = false;
    counted_db() : mdb(4) {}
    batch_status dbapply_pages(const std::vector<batch_entry>& roots,
        const std::vector<page_change>& pages) override {
        ++commits;
        return reject ? batch_status::error : mdb::dbapply_pages(roots, pages);
    }
    lookup_status dblookup_page(const std::string& key, uint64_t id,
        std::string& out) override {
        ++page_reads; return mdb::dblookup_page(key, id, out);
    }
};
static std::shared_ptr<counted_db> memory(db_watchers* observers = nullptr) {
    auto out = std::make_shared<counted_db>();
    require(out->open("", observers), "open memory"); return out;
}
static void ordered(shared_db source) {
    const std::string key("ws\0key", 6);
    require(write(*source, key, "old", 1, true) == status::ok &&
        source->transact_pages({key}, [&](page_transaction& tx) {
            return tx.put(key, 99, "untouched");
        }) == status::ok, "seed");
    auto token = watch(source, key), absent = watch(source, "ws-absent");
    page_workspace workspace(source);
    require(write(workspace, key, "first", 1) == status::ok &&
        read(workspace, key) == encoded("first") &&
        read(*source, key) == encoded("old"), "staged root visibility");
    std::string out;
    require(page(workspace, key, 1, out) && out == "first" &&
        page(workspace, key, 99, out) && out == "untouched",
        "read-through/staged page visibility");
    require(workspace.del(key), "staged erase");
    require(!workspace.get(key, out) && !page(workspace, key, 99, out),
        "erase exposed old family");
    require(write(workspace, key, "recreated", 4) == status::ok &&
        !page(workspace, key, 1, out) && !page(workspace, key, 99, out),
        "recreate resurrected old pages");
    require(workspace.transact_pages({key}, [&](page_transaction& tx) {
        return tx.put(key, 5, "keep");
    }) == status::ok, "KEEP page write");
    require(workspace.set("ws-absent", encoded("x")) &&
        workspace.del("ws-absent") && token->valid(unix_time_ms()) &&
        absent->valid(unix_time_ms()), "staging invalidated source WATCH");
    const size_t batches = workspace.pending_batches();
    require(batches == 6 && workspace.commit() == status::ok &&
        workspace.pending_batches() == 0 && workspace.pending_bytes() == 0 &&
        workspace.pending_records() == 0 && !token->valid(unix_time_ms()) &&
        !absent->valid(unix_time_ms()), "group commit/history/accounting");
    require(read(*source, key) == encoded("recreated") &&
        page(*source, key, 4, out) && out == "recreated" &&
        page(*source, key, 5, out) && out == "keep" &&
        !page(*source, key, 99, out), "committed family mismatch");
    require(workspace.commit() == status::aborted &&
        !workspace.set(key, encoded("late")), "finished workspace reused");
}
static void typed(shared_db source, bool verify_only = false) {
    shared_db view;
    std::shared_ptr<page_workspace> workspace;
    dao::paged_string strings; dao::paged_list lists;
    dao::paged_hash hashes; dao::paged_set sets; dao::paged_zset zsets;
    const std::string large(70000, 'x');
    long long count = 0;
    std::string value;
    if (!verify_only) {
        workspace = std::make_shared<page_workspace>(source); view = workspace;
        require(strings.set(view, "ws-string", large) &&
            strings.append(view, "ws-string", "end", count) &&
            count == 70003 && strings.get(view, "ws-string", value) &&
            value == large + "end", "typed staged String");
        require(lists.push(view, "ws-list", {large, "tail"}, false, false,
            count) && lists.set(view, "ws-list", "1", "changed"),
            "typed staged List");
        require(hashes.hset(view, "ws-hash", "field", large) &&
            hashes.hset(view, "ws-hash", "second", "v"), "typed Hash");
        require(sets.change(view, "ws-set", {large, "second"}, true, count),
            "typed Set");
        dao::zadd_options options; dao::zadd_result result;
        require(zsets.add(view, "ws-zset", {{large, 3}, {"small", 1}},
            options, result), "typed ZSet");
        require(!source->get("ws-string", value), "typed prefix committed");
        require(workspace->commit() == status::ok, "typed workspace commit");
    }
    view = source;
    require(strings.get(view, "ws-string", value) && value == large + "end",
        "committed String");
    std::vector<std::string> items;
    require(lists.range(view, "ws-list", 0, -1, items) &&
        items == std::vector<std::string>({large, "changed"}), "List reopen");
    require(hashes.hget(view, "ws-hash", "field", value) && value == large &&
        hashes.hlen(view, "ws-hash", count) && count == 2, "Hash reopen");
    require(sets.size(view, "ws-set", count) && count == 2, "Set reopen");
    std::vector<zset_member> members;
    require(zsets.range(view, "ws-zset", 0, -1, false, members) &&
        members.size() == 2 && members[0].member == "small" &&
        members[1].member == large && members[1].score == 3, "ZSet reopen");
}
static void discard_scan(shared_db source) {
    require(source->set("ws-scan-old", encoded("old")),
        "scan seed");
    auto token = watch(source, "ws-scan-old");
    {
        page_workspace workspace(source);
        require(workspace.del("ws-scan-old") &&
            workspace.set("ws-scan-new", encoded("new")), "scan staging");
        unsigned long long cursor = 0, next;
        std::set<std::string> seen;
        do {
            std::vector<std::string> keys;
            require(workspace.scan_keys(cursor, 1, keys, next) ==
                db::scan_status::ok, "workspace scan");
            seen.insert(keys.begin(), keys.end()); cursor = next;
        } while (cursor);
        require(!seen.count("ws-scan-old") && seen.count("ws-scan-new"),
            "staged scan membership");
    }
    std::string out;
    require(source->get("ws-scan-old", out) &&
        !source->get("ws-scan-new", out) && token->valid(unix_time_ms()),
        "workspace destructor committed data or WATCH");
}
static void model(shared_db source) {
    struct owner { bool present = false; std::string root;
        std::map<uint64_t, std::string> pages; };
    std::map<std::string, owner> expected;
    std::mt19937 random(7319);
    page_workspace workspace(source);
    for (unsigned step = 0; step < 100; ++step) {
        const auto key = "ws-model-" + std::to_string(random() % 3);
        auto& current = expected[key];
        const auto operation = random() % 4;
        const uint64_t id = 1 + random() % 5;
        const auto value = std::to_string(step);
        require(workspace.transact_pages({key}, [&](page_transaction& tx) {
            auto& root = *tx.roots().find(key);
            if (operation == 0) {
                root.action = action::erase;
                current.present = false; current.pages.clear();
                return true;
            }
            if (operation == 1 || !current.present) {
                root.action = action::write; root.value = encoded(value);
                root.clear_pages = operation == 1;
                if (root.clear_pages) current.pages.clear();
                current.present = true; current.root = root.value;
            }
            if (operation == 3) {
                current.pages.erase(id); return tx.erase(key, id);
            }
            current.pages[id] = value; return tx.put(key, id, value);
        }) == status::ok, "model staging");
        for (const auto& item : expected) {
            std::string out;
            require(workspace.get(item.first, out) == item.second.present &&
                (!item.second.present || out == item.second.root),
                "model root mismatch");
            for (uint64_t page_id = 1; page_id <= 5; ++page_id) {
                const auto found = item.second.pages.find(page_id);
                require(page(workspace, item.first, page_id, out) ==
                    (found != item.second.pages.end()) &&
                    (found == item.second.pages.end() || out == found->second),
                    "model page mismatch");
            }
        }
    }
    require(workspace.commit() == status::ok, "model commit");
    for (const auto& item : expected) {
        std::string out;
        require(source->get(item.first, out) == item.second.present &&
            (!item.second.present || out == item.second.root), "model source");
        for (uint64_t id = 1; id <= 5; ++id) {
            const auto found = item.second.pages.find(id);
            require(page(*source, item.first, id, out) ==
                (found != item.second.pages.end()) &&
                (found == item.second.pages.end() || out == found->second),
                "model committed page");
        }
    }
}
static void faults(bool committing) {
    size_t failures = 0; bool success = false;
    const std::string added(200, 'n');
    const std::string second = encoded("second"), created = encoded("created");
    for (long fail = 0; fail < 220; ++fail) {
        observer notices; db_watchers observers;
        observers.add_watcher(&notices);
        auto source = memory(&observers);
        require(write(*source, "fault", "old", 1, true) == status::ok,
            "fault seed");
        auto token = watch(source, "fault");
        page_workspace workspace(source);
        require(write(workspace, "fault", "first", 1) == status::ok,
            "fault first stage");
        if (committing) require(write(workspace, "fault", "second", 2, true) ==
            status::ok, "fault second stage");
        const auto retained = committing ? "second" : "first";
        notices.calls = 0; source->page_reads = 0;
        status result = status::error;
        budget = fail;
        try {
            result = committing ? workspace.commit() :
                workspace.transact_pages({"fault", added},
                    [&](page_transaction& tx) {
                        auto& root = *tx.roots().find("fault");
                        root.action = action::write; root.value = second;
                        if (!tx.reset_pages("fault")) return false;
                        auto& extra = *tx.roots().find(added);
                        extra.action = action::write; extra.value = created;
                        return tx.put("fault", 2, "second") &&
                            tx.put(added, 3, "created");
                    });
        } catch (const std::bad_alloc&) {}
        budget = -1;
        if (result == status::ok) success = true;
        else ++failures;
        std::string out;
        if (committing && result == status::ok) {
            require(!token->valid(unix_time_ms()) && notices.calls == 2 &&
                source->page_reads == 0 && read(*source, "fault") ==
                encoded(retained), "commit success/observers/old page reads");
        } else {
            require(token->valid(unix_time_ms()) && notices.calls == 0 &&
                read(*source, "fault") == encoded("old"),
                "failed operation committed source prefix");
            if (!committing && result == status::ok) {
                require(read(workspace, "fault") == second &&
                    !page(workspace, "fault", 1, out) &&
                    page(workspace, "fault", 2, out) &&
                    read(workspace, added) == created, "stage success");
            } else {
                require(read(workspace, "fault") == encoded(retained) &&
                    page(workspace, "fault", committing ? 2 : 1, out) &&
                    out == retained && !workspace.get(added, out) &&
                    workspace.pending_batches() == (committing ? 2U : 1U),
                    "stage failure changed view");
            }
            require(workspace.commit() == status::ok, "failure retry");
        }
    }
    require(success && failures > 10, "fault sweep missed allocation paths");
}
static void migration(shared_db source) {
    value_record record; record.header.kind = value_kind::list;
    record.list_value = {std::string(70000, 'l'), "tail"};
    std::string original;
    require(source->value_codec().encode(record, original) ==
        codec_status::ok &&
        source->set("ws-legacy", original), "legacy seed");
    auto token = watch(source, "ws-legacy");
    auto workspace = std::make_shared<page_workspace>(source);
    shared_db view = workspace;
    dao::paged_list list; long long length = 0;
    require(list.length(view, "ws-legacy", length) && length == 2 &&
        read(*source, "ws-legacy") == original &&
        workspace->pending_batches() == 1, "staged legacy migration");
    require(workspace->commit() == status::ok && token->valid(unix_time_ms()) &&
        read(*source, "ws-legacy").size() <= 16384, "migration WATCH changed");
    std::vector<std::string> values;
    require(list.range(source, "ws-legacy", 0, -1, values) &&
        values == record.list_value, "migration values");
}
static void local_watch() {
    auto source = memory();
    require(source->set("local-watch", encoded("old")), "watch seed");
    auto original = watch(source, "local-watch");
    watch_token local;
    {
        auto workspace = std::make_shared<page_workspace>(source);
        local = watch(workspace, "local-watch");
        require(local->valid(unix_time_ms()) &&
            workspace->set("local-watch", encoded("new")) &&
            !local->valid(unix_time_ms()) && original->valid(unix_time_ms()),
            "local/source WATCH ownership mixed");
        require(workspace->commit() == status::ok &&
            !original->valid(unix_time_ms()), "source WATCH commit");
    }
    require(!local->valid(unix_time_ms()), "local epoch lost on destruction");
}
static void rejection() {
    auto source = memory();
    page_workspace workspace(source);
    bool rejected = false;
    try {
        auto alias = shared_db(&workspace, [](db*) {});
        page_workspace nested(alias);
    } catch (const std::invalid_argument&) { rejected = true; }
    require(rejected, "nested workspace accepted");
    const std::string large(16385, 'x');
    require(!workspace.set("oversize", large) &&
        workspace.pending_batches() == 0, "oversized raw root accepted");
    require(write(workspace, "discard", "pending", 1) == status::ok,
        "discard seed");
    workspace.discard(); std::string out;
    require(workspace.commit() == status::aborted &&
        !source->get("discard", out), "explicit discard committed");
}
static void limits_observers() {
    observer failing, healthy; failing.throwing = true;
    db_watchers observers;
    observers.add_watcher(&failing); observers.add_watcher(&healthy);
    auto source = memory(&observers);
    {
        page_workspace empty(source, 0, 0);
        require(empty.commit() == status::ok, "read-only reserved controls");
    }
    {
        page_workspace limited(source, 56, 2);
        require(!limited.set("limit", encoded("x")) &&
            limited.pending_bytes() == 0 && limited.commit() == status::ok,
            "budget rejection retained state");
    }
    page_workspace workspace(source);
    require(workspace.set("notice-a", encoded("a")) &&
        workspace.set("notice-b", encoded("b")) &&
        failing.calls == 0 && healthy.calls == 0, "premature observer event");
    source->reject = true;
    require(workspace.commit() == status::error && healthy.calls == 0,
        "rejected engine commit emitted events");
    source->reject = false;
    const auto before = source->commits;
    require(workspace.commit() == status::ok && source->commits == before + 1 &&
        failing.calls == 2 && healthy.calls == 2,
        "observer allocation failure suppressed later committed events");
    page_workspace unsupported(source);
    healthy.page_capable = false;
    require(unsupported.set("unsupported", encoded("x")) &&
        unsupported.commit() == status::unsupported &&
        source->commits == before + 1 && healthy.calls == 2,
        "observer rejection attempted a source write");
    healthy.page_capable = true;
    require(unsupported.commit() == status::ok && healthy.calls == 3,
        "observer capability retry failed");
}
static void cross_store(shared_db source) {
    const std::string first = "ws-cross-a";
    std::string second = "ws-cross-b";
    const auto shard = [](const std::string& key) {
        return acl_hash_crc32(key.data(), key.size()) % 2;
    };
    while (shard(first) == shard(second)) second += 'x';
    require(source->set(first, encoded("old-a")) &&
        source->set(second, encoded("old-b")), "cross-store seed");
    auto a = watch(source, first), b = watch(source, second);
    page_workspace workspace(source);
    require(workspace.set(first, encoded("new-a")) &&
        workspace.set(second, encoded("new-b")) &&
        workspace.commit() == status::unsupported &&
        read(*source, first) == encoded("old-a") &&
        read(*source, second) == encoded("old-b") &&
        a->valid(unix_time_ms()) && b->valid(unix_time_ms()) &&
        read(workspace, first) == encoded("new-a"),
        "cross-store commit exposed a prefix or lost pending state");
}
int main(int argc, char** argv) {
    acl::acl_cpp_init();
    const std::string backend = argc > 1 ? argv[1] : "mdb";
    shared_db source;
    if (backend == "mdb") source = db::create_mdb(4);
    else if (backend == "tbb") source = db::create_mdb_tbb(4);
    else if (backend == "rdb") source = db::create_rdb();
    else if (backend == "wdb") source = db::create_wdb();
    require(source && source->open(argc > 2 ? argv[2] : ""), "backend open");
    if (argc > 3 && std::string(argv[3]) == "--cross-store") {
        cross_store(source); return 0;
    }
    if (argc > 3) { typed(source, true); return 0; }
    ordered(source); discard_scan(source); model(source);
    migration(source); typed(source);
    if (backend == "mdb") {
        faults(false); faults(true); local_watch();
        rejection(); limits_observers();
    }
    std::cout << "Page workspace passed: " << backend << '\n';
}
