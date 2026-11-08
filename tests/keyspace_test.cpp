#include "stdafx.h"
#include "dao/json/json_scan.h"
#include "dao/json/json_string.h"
#include "dao/paged_string.h"
#include "dao/paged_hash.h"
#include "dao/paged_list.h"
#include "dao/paged_set.h"
#include "dao/paged_zset.h"
#include "db/page_workspace.h"
#include "db/mdb/mdb.h"
#include "coder/list_page.h"
#include "coder/map_page.h"
#include "coder/zset_page.h"
#include "coder/string_page.h"
#include <climits>
#include <cstdlib>
#include <iostream>
#include <new>
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
void operator delete(void* p, std::size_t) noexcept { ::operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept { ::operator delete(p); }
#endif
using namespace pkv;
static void require(bool ok, const char* message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}
class scan_source : public mdb {
public:
    scan_source() : mdb(4) {}
    bool reject = false, stuck = false;
    size_t payload_reads = 0;
protected:
    scan_status dbscan_keys(unsigned long long cursor, size_t count,
        std::vector<std::string>& keys, unsigned long long& next) override {
        if (reject && cursor) return scan_status::error;
        const auto status = mdb::dbscan_keys(cursor, count, keys, next);
        if (stuck && cursor) next = cursor;
        return status;
    }
    lookup_status dblookup_page(const std::string&, uint64_t,
        std::string&) override {
        ++payload_reads; return lookup_status::error;
    }
};
static std::unordered_set<std::string> scan(shared_db db,
    const std::string& pattern) {
    dao::json_scan data;
    std::unordered_set<std::string> out;
    require(data.all_keys(db, pattern, out), "KEYS scan"); return out;
}
static void traversal(shared_db database) {
    dao::paged_string data;
    dao::json_string old;
    std::unordered_set<std::string> expected;
    const std::string value(70000, 'x');
    for (unsigned i = 0; i < 600; ++i) {
        const auto key = "key:" + std::to_string(i);
        require(data.set(database, key, i ? "small" : value), "KEYS seed");
        expected.insert(key);
    }
    require(old.set(database, "legacy", value), "KEYS legacy seed");
    expected.insert("legacy");
    const std::string binary("key:\0\xff", 6);
    require(data.set(database, binary, "binary") &&
        data.set(database, "", "empty"), "KEYS binary seed");
    expected.insert(binary); expected.insert("");
    std::string before, after;
    require(database->get("legacy", before), "KEYS legacy snapshot");
    std::vector<watch_token> watched;
    require(database->watch({"key:0", "legacy"}, watched) ==
        db::batch_status::ok, "KEYS WATCH");
    require(scan(database, "*") == expected &&
        scan(database, std::string("key:\0?", 6)).count(binary) &&
        scan(database, "").count("") && scan(database, "absent").empty(),
        "KEYS exhaustive binary match");
    require(database->get("legacy", after) && before == after &&
        watched[0]->valid(0) && watched[1]->valid(0), "KEYS changed source");
    auto workspace = std::make_shared<page_workspace>(database);
    shared_db staged = workspace;
    require(data.set(staged, "key:0", "changed") &&
        data.set(staged, "staged", "new") && staged->del("key:1"),
        "KEYS workspace staging");
    expected.erase("key:1"); expected.insert("staged");
    require(scan(staged, "*") == expected, "KEYS workspace membership");
    workspace->discard();
    require(scan(database, "staged").empty() &&
        scan(database, "key:1").size() == 1, "KEYS workspace leaked");
}
static void allocation_and_errors() {
    auto source = std::make_shared<scan_source>();
    shared_db database = source;
    require(database->open(""), "KEYS error database");
    dao::paged_string data;
    for (unsigned i = 0; i < 400; ++i)
        require(data.set(database, "key:" + std::to_string(i), "v"),
            "KEYS error seed");
    require(data.set(database, "large", std::string(70000, 'x')),
        "KEYS payload seed");
    dao::json_scan query;
    std::unordered_set<std::string> out;
    budget = LONG_MAX;
    require(query.all_keys(database, "*", out), "KEYS allocation control");
    const auto count = LONG_MAX - budget;
    budget = -1;
    require(!source->payload_reads, "KEYS read native payload pages");
    for (long position = 0; position <= count; ++position) {
        out = {"old-result"}; budget = position;
        try {
            const bool ok = query.all_keys(database, "*", out);
            require(ok ? out.size() == 401 : out.empty(),
                "KEYS returned partial allocation result");
        } catch (const std::bad_alloc&) {
            require(out.empty(), "KEYS exception exposed partial result");
        }
        budget = -1;
    }
    source->reject = true;
    require(!query.all_keys(database, "*", out) && out.empty() &&
        query.status() == dao::dao_base::read_status::storage_error,
        "KEYS lost scan error");
    source->reject = false; source->stuck = true;
    require(!query.all_keys(database, "*", out) && out.empty(),
        "KEYS accepted nonadvancing cursor");
    source->stuck = false;
    require(scan(database, "*").size() == 401, "KEYS failure damaged DB");
    std::cout << "KEYS allocation positions: " << count + 1 << '\n';
}
static void coherent(shared_db database) {
    dao::paged_string data;
    require(data.set(database, "pair:a", "x"), "KEYS pair seed");
    std::thread writer([&] {
        for (unsigned i = 0; i < 200; ++i) {
            const std::string from = i % 2 ? "pair:b" : "pair:a";
            const std::string to = i % 2 ? "pair:a" : "pair:b";
            require(database->transact({from, to}, [&](db::batch_view roots) {
                auto& a = *roots.find(from); auto& b = *roots.find(to);
                if (!a.found) return false;
                b.value = a.value; b.action = db::update_action::write;
                a.action = db::update_action::erase; return true;
            }) == db::batch_status::ok, "KEYS concurrent transfer");
        }
    });
    for (unsigned i = 0; i < 200; ++i)
        require(scan(database, "pair:*").size() == 1,
            "KEYS observed torn keyspace");
    writer.join();
}
class metadata_probe : public dao::dao_base {
public:
    bool inspect(shared_db& db, const std::string& bytes,
        record_header& header, bool& native, long long now) {
        return decode_metadata(db, bytes, header, native, now);
    }
    bool owning(shared_db& db, const std::string& bytes, long long now) {
        return decode(db, bytes, now);
    }
    bool has_view() const { return bool(view_); }
    const record_header& header() const { return view_->header(); }
    bool payload(value_record& out) { return materialize(out); }
};

static void metadata() {
    std::vector<std::pair<std::string, value_kind>> roots;
    std::string bytes;
    string_root string;
    string.length = 70000;
    string.pages = string_page_codec::pages_for(string.length);
    require(string_page_codec::encode_root(string, bytes) ==
        string_page_status::ok, "metadata String encode");
    roots.emplace_back(bytes, value_kind::string);
    string.length = 1024; string.pages = 0;
    string.inline_value.assign(1024, 'x');
    require(string_page_codec::encode_root(string, bytes) ==
        string_page_status::ok, "metadata inline String encode");
    roots.emplace_back(bytes, value_kind::string);
    list_root list;
    list.elements = list.head = list.tail = list.last_id = 1;
    require(list_page_codec::encode_root(list, bytes) == list_page_status::ok,
        "metadata List encode");
    roots.emplace_back(bytes, value_kind::list);
    map_root map;
    map.elements = map.tree = map.last_id = 1;
    for (bool set : {false, true}) {
        map.kind = set ? map_kind::set : map_kind::hash;
        require(map_page_codec::encode_root(map, bytes) == map_page_status::ok,
            "metadata Hash/Set encode");
        roots.emplace_back(bytes, set ? value_kind::set : value_kind::hash);
    }
    zset_root sorted;
    sorted.elements = sorted.members = 1;
    sorted.scores = sorted.last_id = 2;
    require(zset_page_codec::encode_root(sorted, bytes) == map_page_status::ok,
        "metadata ZSet encode");
    roots.emplace_back(bytes, value_kind::zset);
    auto codec = default_record_codec();
    auto database = db::create_mdb(2);
    require(database->open(""), "metadata probe DB");
    metadata_probe probe, ordinary;
    record_header inspected;
    bool native;
    value_record legacy, actual;
    legacy.header.kind = value_kind::hash;
    legacy.header.expires_at_ms = 1;
    legacy.hash_value = {{"field", "value"}};
    std::string encoded;
    require(codec->encode(legacy, encoded) == codec_status::ok &&
        probe.inspect(database, encoded, inspected, native, 0) && !native &&
        probe.has_view() && probe.payload(actual) &&
        actual.hash_value == legacy.hash_value,
        "metadata open lost owning legacy payload");
    for (const auto& root : roots) {
        record_header header;
        budget = 0;
        const auto status = codec->stored_header(root.first, header);
        const bool opened = probe.inspect(database, root.first, inspected,
            native, 0);
        std::unique_ptr<record_view> view;
        const auto owning = codec->decode_stored(root.first, view);
        budget = -1;
        require(status == codec_status::ok && header.kind == root.second &&
            owning == codec_status::out_of_memory && !view,
            "native header inspection allocated an owning view");
        require(opened && native && !probe.has_view() &&
            inspected.kind == root.second,
            "DAO metadata inspection allocated/retained a native view");
        for (size_t i = 0; i < root.first.size(); ++i) {
            bytes = root.first; bytes[i] ^= 0xff;
            const auto metadata_status = codec->stored_header(bytes, header);
            const auto view_status = codec->decode_stored(bytes, view);
            require(metadata_status == view_status &&
                (view_status != codec_status::ok ||
                    (header.kind == view->header().kind &&
                        header.expires_at_ms == view->header().expires_at_ms)),
                "metadata/full-view validation mismatch");
            const bool old_ok = ordinary.owning(database, bytes, 0);
            const bool new_ok = probe.inspect(database, bytes, inspected,
                native, 0);
            require(old_ok == new_ok && ordinary.status() == probe.status()
                && (!old_ok || (ordinary.header().kind == inspected.kind &&
                    ordinary.header().expires_at_ms ==
                        inspected.expires_at_ms)),
                "DAO metadata error/expiry semantics changed");
        }
    }
}
static void metadata_expiry_and_type() {
    auto database = db::create_mdb(2);
    require(database->open(""), "expiry/type metadata DB");
    std::vector<std::string> roots;
    std::string bytes;
    list_root list;
    list.elements = list.head = list.tail = list.last_id = 1;
    require(list_page_codec::encode_root(list, bytes) == list_page_status::ok,
        "live List metadata"); roots.push_back(bytes);
    zset_root sorted;
    sorted.elements = sorted.members = 1;
    sorted.scores = sorted.last_id = 2;
    require(zset_page_codec::encode_root(sorted, bytes) == map_page_status::ok,
        "live ZSet metadata"); roots.push_back(bytes);
    map_root map;
    map.elements = map.tree = map.last_id = 1;
    for (auto kind : {map_kind::hash, map_kind::set}) {
        map.kind = kind;
        require(map_page_codec::encode_root(map, bytes) == map_page_status::ok,
            "live Hash/Set metadata"); roots.push_back(bytes);
    }
    list.expires_at_ms = sorted.expires_at_ms = map.expires_at_ms = 0;
    require(list_page_codec::encode_root(list, bytes) == list_page_status::ok,
        "expired List metadata"); roots.push_back(bytes);
    require(zset_page_codec::encode_root(sorted, bytes) == map_page_status::ok,
        "expired ZSet metadata"); roots.push_back(bytes);
    for (auto kind : {map_kind::hash, map_kind::set}) {
        map.kind = kind;
        require(map_page_codec::encode_root(map, bytes) == map_page_status::ok,
            "expired Hash/Set metadata"); roots.push_back(bytes);
    }
    dao::paged_hash hashes;
    dao::paged_list lists;
    dao::paged_set sets;
    dao::paged_zset sorted_sets;
    for (size_t i = 0; i < roots.size(); ++i) {
        require(database->set("root", roots[i]), "wrong-type metadata seed");
        long long count = -1;
        bool ok;
        dao::dao_base* status;
        switch (i % 4) {
        case 0: ok = hashes.hlen(database, "root", count); status = &hashes;
            break;
        case 1: ok = lists.length(database, "root", count); status = &lists;
            break;
        case 2: ok = sets.size(database, "root", count); status = &sets; break;
        default: ok = sorted_sets.size(database, "root", count);
            status = &sorted_sets;
        }
        require(i < 4 ? (!ok && status->status() ==
            dao::dao_base::read_status::wrong_type) : (ok && count == 0),
            "metadata changed expiry-before-wrong-type precedence");
    }
}

int main() {
    acl::acl_cpp_init(); metadata(); metadata_expiry_and_type();
    allocation_and_errors();
    for (bool tbb : {false, true}) {
#ifndef HAS_TBB
        if (tbb) continue;
#endif
        auto database = tbb ? db::create_mdb_tbb(4) : db::create_mdb(4);
        require(database->open(""), "KEYS backend open");
        traversal(database); coherent(database);
    }
    std::cout << "KEYS storage checks passed\n";
}
