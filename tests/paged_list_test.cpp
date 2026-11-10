#include "db/db_watcher.h"
#include "stdafx.h"
#include "coder/compact_list_root.h"
#include "dao/paged_list.h"
#include "dao/paged_string.h"
#include "dao/paged_hash.h"
#include "dao/paged_set.h"
#include "dao/paged_zset.h"
#include "dao/json/json_list.h"
#include "dao/json/json_key.h"
#include "dao/json/json_string.h"
#include "fixture_record_codec.h"
#include "db/mdb/mdb.h"
#include <cstdlib>
#include <iostream>
#include <random>
#include <thread>
#include <atomic>
#include <chrono>

using namespace pkv;
static const long long deadline = 4102444800123LL;

static void require(bool ok, const char* reason) {
    if (!ok) { std::cerr << reason << '\n'; std::exit(1); }
}

static shared_db memory() {
    auto database = db::create_mdb(4);
    require(database->open(""), "open");
    return database;
}

static std::vector<std::string> contents(dao::list& list, shared_db database,
    const std::string& key) {
    std::vector<std::string> result;
    require(list.range(database, key, 0, -1, result), "read contents");
    return result;
}

static void root(shared_db database, const std::string& key, size_t count) {
    std::string bytes;
    require(database->get(key, bytes) == (count != 0), "root presence");
    if (!count) return;
    if (compact_list_root::matches(bytes)) {
        require(compact_list_root::view(bytes).count() == count, "compact List size");
        return;
    }
    list_root metadata;
    require(bytes.size() <= list_page_codec::extended_root_bytes &&
        list_page_codec::decode_root(bytes, metadata) == list_page_status::ok &&
        metadata.elements == count, "List still stored as whole encoded value");
}

static void test_model() {
    shared_db native = memory(), legacy = memory();
    dao::paged_list pages;
    dao::json_list json;
    std::mt19937 random(14273);
    const std::vector<std::string> samples = {
        "", "a", "b", std::string("a\0b", 3), std::string(40000, 'x')};
    for (unsigned step = 0; step < 600; ++step) {
        const auto& value = samples[random() % samples.size()];
        const long long index = static_cast<long long>(random() % 15) - 7;
        const bool left = random() % 2 != 0;
        long long first = 0, second = 0;
        bool a = false, b = false;
        const auto operation = random() % 9;
        if (operation == 0) {
            a = pages.push(native, "list", {value, "a"}, left, false, first);
            b = json.push(legacy, "list", {value, "a"}, left, false, second);
        } else if (operation == 1) {
            dao::list_result x, y;
            const auto count = random() % 4;
            a = pages.pop(native, "list", left, count, x);
            b = json.pop(legacy, "list", left, count, y);
            require(x.found == y.found && x.values == y.values, "pop model");
        } else if (operation == 2) {
            const auto text = std::to_string(index);
            a = pages.set(native, "list", text, value);
            b = json.set(legacy, "list", text, value);
        } else if (operation == 3) {
            a = pages.insert(native, "list", left, "a", value, first);
            b = json.insert(legacy, "list", left, "a", value, second);
        } else if (operation == 4) {
            a = pages.remove(native, "list", index, value, first);
            b = json.remove(legacy, "list", index, value, second);
        } else if (operation == 5) {
            a = pages.trim(native, "list", index, -1);
            b = json.trim(legacy, "list", index, -1);
        } else if (operation == 6) {
            list_search options;
            options.rank = left ? -1 : 1; options.count = random() % 3;
            std::vector<long long> x, y;
            a = pages.positions(native, "list", value, options, x);
            b = json.positions(legacy, "list", value, options, y);
            require(x == y, "positions model");
        } else if (operation == 7) {
            dao::list_result x, y;
            a = pages.index(native, "list", std::to_string(index), x);
            b = json.index(legacy, "list", std::to_string(index), y);
            require(x.found == y.found && x.values == y.values, "index model");
        } else {
            a = pages.push(native, "list", {value}, left, true, first);
            b = json.push(legacy, "list", {value}, left, true, second);
        }
        require(a == b && first == second, "List result diverged");
        if (!a) require(pages.status() == json.status(), "error precedence");
        const auto actual = contents(pages, native, "list");
        require(actual == contents(json, legacy, "list"), "members diverged");
        root(native, "list", actual.size());
    }
}

static void test_migration() {
    shared_db database = memory();
    fixture_record_codec codec;
    value_record old;
    old.header.kind = value_kind::list;
    old.header.expires_at_ms = deadline;
    old.list_value.assign(1000, "small");
    old.list_value.push_back(std::string(1024 * 1024, 'z'));
    std::string encoded;
    require(codec.encode(old, encoded) == codec_status::ok &&
        database->set("legacy", encoded), "legacy seed");
    dao::paged_list pages;
    require(!pages.set(database, "legacy", "bad", "replacement") &&
        pages.status() == dao::dao_base::read_status::invalid_integer,
        "migration error precedence");
    std::string after;
    require(database->get("legacy", after) && after == encoded,
        "failed operation partially migrated legacy value");
    long long count;
    require(pages.length(database, "legacy", count) && count == 1001,
        "legacy migration length");
    root(database, "legacy", 1001);
    require(contents(pages, database, "legacy") == old.list_value,
        "legacy migration contents");
    dao::json_key keys;
    require(keys.expiry_time(database, "legacy", count) &&
        count == old.header.expires_at_ms, "migration expiry");
}

static void test_transfer() {
    shared_db database = memory(), target = memory();
    dao::paged_list pages;
    dao::json_key keys;
    dao::json_string strings;
    long long length;
    const std::vector<std::string> expected = {
        "a", "", std::string(40000, 'x')};
    require(pages.push(database, "from", expected, false, false, length) &&
        keys.set_expiry(database, "from", deadline, 0), "transfer seed");
    bool done;
    require(keys.copy(database, "from", target, "copy", false, done) && done,
        "cross DB COPY");
    require(contents(pages, target, "copy") == expected &&
        keys.expiry_time(target, "copy", length) && length == deadline,
        "COPY contents/expiry");
    require(keys.copy(database, "from", target, "copy", false, done) && !done,
        "COPY without REPLACE");
    require(pages.set(target, "copy", "0", "independent") &&
        contents(pages, database, "from") == expected, "COPY aliased pages");
    require(strings.set(database, "wrong", "string") &&
        keys.rename(database, "from", "wrong", true, done) && !done,
        "RENAMENX overwrote live value");
    require(keys.rename(database, "from", "renamed", false, done) && done &&
        contents(pages, database, "renamed") == expected, "RENAME");
    root(database, "from", 0);
    require(keys.rename(database, "renamed", "renamed", false, done) && done &&
        keys.rename(database, "renamed", "renamed", true, done) && !done,
        "same-key RENAME policy");
    dao::list_result result;
    require(pages.move(database, "missing", "wrong", true, false, result) &&
        !result.found, "missing source checked destination type");
    require(!pages.move(database, "renamed", "wrong", true, false, result) &&
        !result.found && result.values.empty() &&
        contents(pages, database, "renamed") == expected, "LMOVE rollback");
    require(pages.move(database, "renamed", "to", true, false, result) &&
        result.found && result.values == std::vector<std::string>{"a"},
        "LMOVE result");
    std::string selected;
    require(pages.mpop(database, {"missing", "to", "wrong"}, true, 2,
        selected, result) && selected == "to" && result.found &&
        result.values == std::vector<std::string>{"a"}, "LMPOP order");
    require(!pages.mpop(database, {"missing", "wrong", "renamed"}, true, 2,
        selected, result) && selected.empty() && !result.found &&
        result.values.empty(), "LMPOP wrong-type precedence");
}

static void test_native_open_metadata() {
    const auto invalid = dao::dao_base::read_status::invalid_record;
    const auto unsupported = dao::dao_base::read_status::unsupported_format;
    for (unsigned version = 1; version <= 2; ++version) {
        shared_db database = memory();
        dao::paged_list list;
        auto seed = [&](long long expiry) {
            require(database->transact_pages({"native"},
                [&](page_transaction& tx) {
                    list_store store(tx, "native");
                    if (!store.open()) return false;
                    store.initialize(expiry);
                    const std::string value(20, 'a');
                    if (!store.push(list_values_view(&value, 1), false) ||
                        !store.finish()) return false;
                    auto& entry = *tx.roots().find("native");
                    list_root root;
                    if (list_page_codec::decode_root(entry.value, root) !=
                        list_page_status::ok) return false;
                    if (version == 1) root.last_id = 0;
                    return list_page_codec::encode_root(root, entry.value) ==
                        list_page_status::ok;
                }) == db::batch_status::ok, "native metadata fixture");
        };
        long long length = -1;
        seed(deadline);
        require(list.push(database, "native", {"b"}, false, true, length) &&
            length == 2, "live native push");
        dao::json_key keys;
        require(keys.expiry_time(database, "native", length) &&
            length == deadline, "native push lost expiry");
        seed(1);
        require(list.push(database, "native", {"b"}, false, true, length) &&
            length == 0, "expired native pushx");
        std::string bytes;
        require(!database->get("native", bytes), "expired native retained");
        seed(1);
        require(list.push(database, "native", {"fresh"}, false, false, length) &&
            length == 1 && contents(list, database, "native") ==
                std::vector<std::string>{"fresh"} &&
            keys.expiry_time(database, "native", length) && length == -1,
            "expired native recreation retained contents or expiry");
        seed(1);
        require(database->get("native", bytes), "native root bytes");
        // Expiry never hides a corrupt header or unknown version.
        auto bad = bytes; bad[4] = 1;
        for (const auto& malformed : {bad, bytes.substr(0, bytes.size() - 1)}) {
            require(database->set("bad", malformed) &&
                !list.push(database, "bad", {"b"}, false, false, length) &&
                list.status() == invalid, "invalid expired native accepted");
            std::string after;
            require(database->get("bad", after) && after == malformed,
                "invalid native mutation published");
        }
        bytes[3] = 127;
        require(database->set("bad", bytes) &&
            !list.push(database, "bad", {"b"}, false, false, length) &&
            list.status() == unsupported, "future native accepted");
    }
}

static void test_future_page_transfer() {
    shared_db database = memory();
    dao::json_key keys;
    require(database->transact_pages({"future"}, [](page_transaction& tx) {
        list_store store(tx, "future");
        const std::string value = "v";
        if (!store.open() || !store.push(list_values_view(&value, 1), false) ||
            !store.finish()) return false;
        std::string bytes;
        if (tx.lookup("future", 1, bytes) != db::lookup_status::found)
            return false;
        bytes[3] = 4;
        return tx.put("future", 1, bytes);
    }) == db::batch_status::ok, "future page fixture");
    bool copied, renamed;
    require(!keys.copy(database, "future", database, "copy", true, copied) &&
        !copied && keys.status() ==
            dao::dao_base::read_status::unsupported_format,
        "COPY lost future page error classification");
    require(!keys.rename(database, "future", "renamed", false, renamed) &&
        !renamed && keys.status() ==
            dao::dao_base::read_status::unsupported_format,
        "RENAME lost future page error classification");
    std::string bytes;
    require(database->get("future", bytes) && !database->get("copy", bytes) &&
        !database->get("renamed", bytes), "future page transfer changed roots");
}

static void test_compact_root() {
    shared_db database = memory();
    dao::paged_list list;
    dao::json_key keys;
    const std::vector<std::string> fixed{"abc", "def", "abc"};
    long long fixed_length;
    std::string fixed_root;
    dao::list_result fixed_result;
    require(list.push(database, "fixed", fixed, false, false, fixed_length) &&
        fixed_length == 3 && list.index(database, "fixed", "2", fixed_result) &&
        fixed_result.found && fixed_result.values == std::vector<std::string>{"abc"} &&
        database->get("fixed", fixed_root) && compact_list_root::matches(fixed_root),
        "fixed-width List index with repeated values");
    const auto field = fixed_root.find("def");
    require(field != std::string::npos && field > 0, "fixed-width List fixture");
    fixed_root[field - 1] = 2;
    require(database->set("bad-fixed", fixed_root) &&
        !list.index(database, "bad-fixed", "0", fixed_result) &&
        list.status() == dao::dao_base::read_status::invalid_record,
        "fixed-width List accepted invalid length");
    const std::string binary("\0\xff", 2);
    long long length;
    std::string bytes, after;
    require(list.push(database, "small", {binary, ""}, false, false, length) &&
        length == 2 && keys.set_expiry(database, "small", deadline, 0) &&
        list.push(database, "small", {"one", "two"}, true, true, length) &&
        length == 4 && database->get("small", bytes) &&
        compact_list_root::matches(bytes) &&
        compact_list_root::view(bytes).deadline() == deadline, "compact List push");
    dao::list_result result;
    require(list.index(database, "small", "-2", result) && result.found &&
        result.values == std::vector<std::string>{binary} &&
        database->get("small", after) && bytes == after, "compact List index");
    std::vector<std::string> range;
    require(list.push(database, "push", {binary}, false, false, length) &&
        keys.set_expiry(database, "push", deadline, 0) &&
        list.push(database, "push", {"tail", ""}, false, true, length) &&
        list.push(database, "push", {"first", "second"}, true, true, length) &&
        length == 5 && list.range(database, "push", 0, -1, range) &&
        range == std::vector<std::string>{"second", "first", binary, "tail", ""} &&
        database->get("push", after) && compact_list_root::matches(after) &&
        compact_list_root::view(after).deadline() == deadline,
        "compact List push on both sides preserves TTL");
    require(list.length(database, "small", length) && length == 4 &&
        list.range(database, "small", -3, -1, range) &&
        range == std::vector<std::string>{"one", binary, ""} &&
        list.range(database, "small", 5, 7, range) && range.empty() &&
        database->get("small", after) && bytes == after, "compact List length and range");
    require(list.push(database, "search", {binary, "", binary, binary},
        false, false, length) && database->get("search", after) &&
        compact_list_root::matches(after), "compact List search seed");
    std::vector<long long> positions;
    list_search options;
    options.rank = -2;
    options.count = 0;
    require(list.positions(database, "search", binary, options, positions) &&
        positions == std::vector<long long>{2, 0}, "compact List reverse search");
    options.maxlen = 3;
    require(list.positions(database, "search", binary, options, positions) &&
        positions == std::vector<long long>{2}, "compact List MAXLEN");
    std::string searched;
    require(database->get("search", searched) && searched == after,
        "compact List search changed the root");
    require(keys.set_expiry(database, "search", deadline, 0) &&
        list.insert(database, "search", false, binary, "inserted", length) &&
        length == 5 && list.range(database, "search", 0, -1, range) &&
        range == std::vector<std::string>{binary, "inserted", "", binary, binary} &&
        database->get("search", searched) && compact_list_root::matches(searched) &&
        compact_list_root::view(searched).deadline() == deadline,
        "compact List insert after first match");
    require(list.insert(database, "search", true, "absent", "", length) &&
        length == -1 && database->get("search", after) && after == searched,
        "compact List missing pivot changed root");
    long long removed;
    require(list.remove(database, "search", -1, binary, removed) && removed == 1 &&
        list.remove(database, "search", 1, binary, removed) && removed == 1 &&
        list.range(database, "search", 0, -1, range) &&
        range == std::vector<std::string>{"inserted", "", binary} &&
        database->get("search", searched) && compact_list_root::matches(searched) &&
        compact_list_root::view(searched).deadline() == deadline,
        "compact List directional removal preserves order and TTL");
    require(list.remove(database, "search", 0, "absent", removed) && removed == 0 &&
        database->get("search", after) && after == searched,
        "compact List missing removal changed root");
    require(list.remove(database, "search", 0, "inserted", removed) && removed == 1 &&
        list.remove(database, "search", 0, "", removed) && removed == 1 &&
        list.remove(database, "search", 0, binary, removed) && removed == 1 &&
        !database->get("search", after), "compact List removal deletes final key");
    require(list.push(database, "move-source", {binary, ""}, false, false, length) &&
        keys.set_expiry(database, "move-source", deadline, 0) &&
        list.move(database, "move-source", "move-source", true, false, result) &&
        result.found && result.values == std::vector<std::string>{binary} &&
        list.move(database, "move-source", "move-target", false, true, result) &&
        result.found && result.values == std::vector<std::string>{binary} &&
        database->get("move-source", after) && compact_list_root::matches(after) &&
        compact_list_root::view(after).deadline() == deadline &&
        database->get("move-target", after) && compact_list_root::matches(after) &&
        compact_list_root::view(after).deadline() == -1,
        "compact List rotate and move preserve source TTL");
    require(keys.set_expiry(database, "move-target", deadline - 1, 0) &&
        list.move(database, "move-source", "move-target", true, false, result) &&
        result.found && result.values == std::vector<std::string>{""} &&
        !database->get("move-source", after) &&
        database->get("move-target", after) && compact_list_root::matches(after) &&
        compact_list_root::view(after).deadline() == deadline - 1,
        "compact List move deletes source and preserves destination TTL");
    std::string selected;
    require(list.mpop(database, {"absent", "move-target", "small"}, false,
        1, selected, result) && selected == "move-target" && result.found &&
        result.values == std::vector<std::string>{""} &&
        database->get("move-target", after) && compact_list_root::matches(after) &&
        compact_list_root::view(after).deadline() == deadline - 1 &&
        compact_list_root::view(after).at(0).str() == binary &&
        database->get("small", after) && after == bytes,
        "compact List multi-pop preserves key priority and TTL");
    bool copied;
    require(keys.copy(database, "small", database, "copy", false, copied) &&
        copied && database->get("copy", after) && bytes == after, "compact List COPY");
    require(list.pop(database, "copy", true, 0, result) && result.found &&
        result.values.empty() && database->get("copy", after) && bytes == after,
        "compact List zero pop");
    require(list.pop(database, "copy", false, 2, result) && result.found &&
        result.values == std::vector<std::string>{"", binary} &&
        database->get("copy", after) && compact_list_root::matches(after) &&
        compact_list_root::view(after).deadline() == deadline, "compact List RPOP");
    bool found;
    require(list.pop_one(database, "copy", true, after, found) && found &&
        after == "two" && list.pop_one(database, "copy", true, after, found) &&
        found && after == "one" && !database->get("copy", after),
        "compact List LPOP deletes last element");
    require(keys.copy(database, "small", database, "copy", false, copied) &&
        copied && list.trim(database, "copy", 1, -2) &&
        database->get("copy", after) && compact_list_root::matches(after) &&
        compact_list_root::view(after).deadline() == deadline &&
        list.range(database, "copy", 0, -1, range) &&
        range == std::vector<std::string>{"one", binary},
        "compact List trim preserves range and TTL");
    require(list.trim(database, "copy", 2, 3) && !database->get("copy", after),
        "compact List empty trim deletes key");
    require(keys.copy(database, "small", database, "copy", false, copied) &&
        copied && !list.set(database, "copy", "9", binary) &&
        list.status() == dao::dao_base::read_status::index_range &&
        database->get("copy", after) && bytes == after, "compact List invalid LSET");
    require(list.set(database, "copy", "-3", "") &&
        list.set(database, "copy", "1", "longer value") &&
        database->get("copy", after) && compact_list_root::matches(after) &&
        compact_list_root::view(after).deadline() == deadline &&
        list.range(database, "copy", 0, -1, range) &&
        range == std::vector<std::string>{"two", "longer value", binary, ""},
        "compact List LSET preserves adjacent values and TTL");
    require(list.set(database, "copy", "0", std::string(256, 'x')) &&
        database->get("copy", after) && !compact_list_root::matches(after) &&
        keys.expiry_time(database, "copy", length) && length == deadline,
        "oversized LSET promotes without losing TTL");
    require(list.push(database, "small",
        std::vector<std::string>(compact_list_root::max_members, "x"),
        false, false, length) && length == compact_list_root::max_members + 4 &&
        database->get("small", after) && !compact_list_root::matches(after) &&
        keys.expiry_time(database, "small", length) && length == deadline,
        "List promotion preserves TTL");
}

static void test_copy_type_race() {
    shared_db database = memory();
    dao::json_string strings;
    require(strings.set(database, "source", "string"), "race seed");
    value_record old;
    old.header.kind = value_kind::list;
    old.list_value = {"v"};
    std::string encoded;
    require(default_record_codec()->encode(old, encoded) == codec_status::ok,
        "race legacy encoding");
    std::atomic<bool> start(false);
    std::thread writer([&] {
        dao::paged_list pages;
        dao::json_string text;
        while (!start.load()) std::this_thread::yield();
        for (unsigned i = 0; i < 400; ++i) {
            require(text.set(database, "source", "string") &&
                database->set("source", encoded), "race replacement");
            std::this_thread::yield();
            long long length;
            require(pages.push(database, "source", {"p"}, false, false,
                length), "race migration");
        }
    });
    dao::json_key keys;
    dao::paged_list pages;
    dao::paged_string target_strings;
    start = true;
    for (unsigned i = 0; i < 400; ++i) {
        bool copied;
        require(keys.copy(database, "source", database, "target", true,
            copied) && copied, "COPY failed during representation changes");
        std::string type;
        require(keys.type(database, "target", type), "race target type");
        if (type == "string") {
            std::string value;
            require(target_strings.get(database, "target", value) && value ==
                "string", "race String snapshot");
        } else {
            const auto values = contents(pages, database, "target");
            require(type == "list" && (values == old.list_value || values ==
                std::vector<std::string>{"v", "p"}), "race List snapshot");
        }
    }
    writer.join();
}

class yielding_pages : public mdb {
public:
    yielding_pages() : mdb(16) {}
    bool yielding = false;
    unsigned page_reads = 0;
protected:
    bool dbget(const std::string& key, std::string& value) override {
        const auto found = mdb::dbget(key, value);
        if (yielding) acl::fiber::yield();
        return found;
    }
    lookup_status dblookup_page(const std::string& key, uint64_t id,
        std::string& value) override {
        const auto status = mdb::dblookup_page(key, id, value);
        if (yielding) { ++page_reads; acl::fiber::yield(); }
        return status;
    }
};

static void test_callback_lifetime_across_yields() {
    acl::acl_cpp_init();
    auto native = std::make_shared<yielding_pages>();
    shared_db database = native;
    require(database->open(""), "yielding DAO open");
    native->yielding = true;
    std::thread worker([&] {
        for (unsigned fiber = 0; fiber < 8; ++fiber) go[&, fiber] {
            dao::paged_string strings;
            dao::paged_list lists;
            dao::paged_hash hashes;
            dao::paged_set sets;
            dao::paged_zset sorted;
            const auto key = std::to_string(fiber);
            const auto page_key = key + "paged-list";
            long long seeded;
            require(lists.push(database, page_key,
                std::vector<std::string>(96, "seed"), false, false, seeded) &&
                seeded == 96, "yielded paged List seed");
            for (unsigned i = 0; i < 20; ++i) {
                const auto token = key + ":" + std::to_string(i);
                long long count;
                std::string value;
                require(strings.set(database, key + "s", token) &&
                    strings.get(database, key + "s", value) && value == token
                    && strings.increment(database, key + "n", 1, false,
                        count) && count == i + 1, "yielded String callback");
                bool applied;
                std::vector<dao::string_result> batch;
                require(strings.mset(database, {{key + "a", token},
                    {key + "b", token}}, false, applied) && applied &&
                    strings.mget(database, {key + "a", key + "b"}, batch) &&
                    batch.size() == 2 && batch[0].found && batch[1].found &&
                    batch[0].value == token && batch[1].value == token,
                    "yielded MSET/MGET callback");
                dao::list_result popped;
                require(lists.push(database, key + "l", {token}, true,
                    false, count) && count == 1 && lists.pop(database,
                        key + "l", true, 1, popped) && popped.found &&
                    popped.values == std::vector<std::string>{token},
                    "yielded List callback");
                bool found;
                dao::list_result indexed;
                std::vector<std::string> ranged;
                require(lists.push(database, page_key, {token}, true, false,
                    count) && count == 97 && lists.index(database, page_key,
                    "0", indexed) && indexed.found &&
                    indexed.values == std::vector<std::string>{token} &&
                    lists.range(database, page_key, 0, 0, ranged) &&
                    ranged == std::vector<std::string>{token} &&
                    lists.pop_one(database, page_key, true, value, found) &&
                    found && value == token &&
                    lists.length(database, page_key, count) && count == 96,
                    "borrowed paged List callbacks survived yielding");
                require(hashes.hset(database, key + "h", "field", token) &&
                    hashes.hget(database, key + "h", "field", value) &&
                    value == token, "yielded Hash callback");
                require(sets.change(database, key + "t", {token}, true,
                    count) && count == 1 && sets.change(database, key + "t",
                        {token}, false, count) && count == 1,
                    "yielded Set callback");
                dao::zadd_result added;
                require(sorted.add(database, key + "z", {{token, double(i)}},
                    dao::zadd_options(), added) && added.added == 1 &&
                    sorted.remove(database, key + "z", {token}, count) &&
                    count == 1, "yielded ZSet callback");
            }
        };
        acl::fiber::schedule();
    });
    worker.join();
    require(native->page_reads != 0, "paged callback yield hook not exercised");
    native->yielding = false;
}

static void test_record_bucket_with_pinned_list() {
    auto database = db::create_mdb(1);
    require(database->open(""), "record bucket List open");
    // All keys share one physical root/family bucket; their logical mutexes
    // differ, allowing sibling changes while the anchor's scope stays pinned.
    std::array<std::string, 8> names;
    size_t remaining = names.size();
    for (unsigned i = 0; remaining; ++i) {
        const auto key = "record-bucket:" + std::to_string(i);
        const auto hash = std::hash<std::string>()(key);
        if (hash % 32) continue;
        const auto slot = (hash % 256) / 32;
        if (names[slot].empty()) { names[slot] = key; --remaining; }
    }
    dao::paged_list list;
    const std::vector<std::string> values(128, "anchor");
    long long length;
    require(list.push(database, names[0], values, false, false, length) &&
        list.push(database, names[1], values, false, false, length),
        "record bucket seed");
    {
        execution_guard admission;
        db::page_lock_scope scope(*database, names[1]);
        dao::paged_list pinned(&scope);
        const auto check = [&] {
            std::string value;
            bool found;
            require(pinned.push(database, names[1], {"round"}, false, false,
                length) && length == 129 && pinned.pop_one(database, names[1],
                    false, value, found) && found && value == "round",
                "record bucket invalidated pinned root/page");
        };
        check();
        for (size_t i = 2; i < names.size(); ++i) {
            require(list.push(database, names[i], values, false, false, length),
                "record bucket sibling insertion");
            check();
        }
        require(database->del(names[0]), "record bucket first erase");
        check();
        require(list.push(database, names[0], values, false, false, length),
            "record bucket first refill");
        check();
        require(database->del(names[0]), "record bucket first erase again");
        for (size_t i = 2; i < names.size(); ++i) {
            require(database->del(names[i]), "record bucket sibling erase");
            check();
        }
    }
    uint64_t count;
    require(database->key_count(count) == db::count_status::ok && count == 1 &&
        contents(list, database, names[1]) == values,
        "overflow-only root survived first erase/refill");
}

static void test_shard_family_growth_with_pinned_list() {
    auto database = db::create_mdb(1);
    require(database->open(""), "directory List open");
    // Distinct low hash bits select distinct family buckets and logical key
    // locks, so unrelated writes can run while the anchor scope is pinned.
    std::array<std::string, 32> names;
    size_t remaining = names.size();
    for (unsigned i = 0; remaining; ++i) {
        const auto key = "directory-list:" + std::to_string(i);
        const auto slot = std::hash<std::string>()(key) % names.size();
        if (names[slot].empty()) { names[slot] = key; --remaining; }
    }
    dao::paged_list list;
    const std::vector<std::string> values(128, "anchor");
    long long length;
    require(list.push(database, names[0], values, false, false, length),
        "directory anchor seed");
    execution_guard admission;
    db::page_lock_scope scope(*database, names[0]);
    dao::paged_list pinned(&scope);
    const auto check = [&] {
        dao::list_result value;
        require(pinned.index(database, names[0], "64", value) &&
            value.found && value.values == std::vector<std::string>{"anchor"},
            "directory growth invalidated pinned page");
    };
    check();
    for (unsigned round = 0; round < 3; ++round) {
        for (size_t i = 1; i < names.size(); ++i) {
            require(list.push(database, names[i], values, false, false, length),
                "directory sibling seed");
            check();
        }
        for (size_t i = names.size(); --i;) {
            require(database->del(names[i]), "directory sibling erase");
            check();
        }
        std::string value;
        bool found;
        require(pinned.push(database, names[0], {"round"}, false, false,
            length) && length == 129 && pinned.pop_one(database, names[0],
                false, value, found) && found && value == "round",
            "pinned List update after directory contraction");
    }
}

static void test_mutation_expiry_index() {
    for (bool tbb : {false, true}) {
#ifndef HAS_TBB
        if (tbb) continue;
#endif
        for (size_t width : {size_t(3), size_t(512)}) {
            auto database = tbb ? db::create_mdb_tbb(4) : memory();
            if (tbb) require(database->open(""), "open TBB List expiry");
            dao::paged_list list;
            dao::json_key keys;
            const std::string key = "expiry-list", member(width, 'a');
            long long length;
            require(list.push(database, key, {member}, false, false, length),
                "List expiry seed");
            const auto due = unix_time_ms() + 150;
            require(keys.set_expiry(database, key, due, 0) &&
                database->rebuild_expiry() == db::scan_status::ok,
                "List expiry index seed");
            {
                execution_guard admission;
                db::page_lock_scope scope(*database, key);
                dao::paged_list scoped(&scope);
                require(scoped.push(database, key, {member}, true, false,
                    length) && length == 2, "scoped List deadline write");
                std::string out;
                bool found;
                require(scoped.pop_one(database, key, false, out, found) &&
                    found && out == member, "scoped List deadline pop");
            }
            require(keys.expiry_time(database, key, length) && length == due,
                "List mutation changed encoded TTL");
            while (unix_time_ms() <= due)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            const auto expired = database->expire_step(4);
            std::string bytes;
            require(expired.ok && expired.removed == 1 &&
                !database->get(key, bytes),
                "List mutation lost active expiry registration");
            require(list.push(database, key, {member}, false, false, length) &&
                keys.expiry_time(database, key, length) && length == -1,
                "List recreation inherited old TTL");
            require(database->expire_step(4).checked == 0,
                "List recreation retained stale deadline");
            const auto another = unix_time_ms() + 150;
            require(keys.set_expiry(database, key, another, 0),
                "List erase expiry seed");
            std::string out;
            bool found;
            require(list.pop_one(database, key, true, out, found) && found &&
                list.push(database, key, {member}, false, false, length),
                "List last-pop recreation");
            while (unix_time_ms() <= another)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            require(database->expire_step(4).checked == 0 &&
                database->get(key, bytes), "last-pop retained stale expiry");
        }
    }
}

class append_payload_observer : public db_watcher {
public:
    unsigned publications = 0, page_payloads = 0;
    bool supports_pages() const override { return true; }
    bool on_set(const std::string&, const std::string&, bool) override { return true; }
    bool on_get(const std::string&, const std::string&, bool) override { return true; }
    bool on_del(const std::string&, bool) override { return true; }
    bool on_pages_database(unsigned, const std::vector<db::batch_entry>& roots,
        const std::vector<page_change>& pages) override {
        require(roots.size() == 1 && !pages.empty(),
            "payload observer received root without List changes");
        ++publications;
        for (const auto& change : pages) {
            if (change.action != db::update_action::write) continue;
            list_page page;
            require(list_page_codec::decode_page(change.value, page) ==
                list_page_status::ok, "observer received append delta bytes");
            ++page_payloads;
        }
        return true;
    }
};

static void test_append_observer_fallback(bool left) {
    append_payload_observer observer;
    db_watchers watchers;
    watchers.add_watcher(&observer);
    auto database = db::create_mdb(4);
    require(database->open("", &watchers), "append observed open");
    dao::paged_list lists;
    const std::string key = "observed-append";
    long long length;
    require(lists.push(database, key, std::vector<std::string>(96, "seed"),
        false, false, length), "append observed seed");
    const auto publications = observer.publications;
    const auto payloads = observer.page_payloads;
    {
        execution_guard admission;
        db::page_lock_scope scope(*database, key);
        dao::paged_list pinned(&scope);
        for (unsigned i = 0; i < 100; ++i)
            require(pinned.push(database, key, {"v"}, left, false, length) &&
                length == 97 + i, "append observer fallback push");
    }
    require(observer.publications == publications + 100 &&
        observer.page_payloads >= payloads + 100,
        "append observer lost full page publications");
    auto values = contents(lists, database, key);
    std::vector<std::string> expected(96, "seed");
    expected.insert(left ? expected.begin() : expected.end(), 100, "v");
    require(values == expected, "observed push final content");
}

static void test_native_append_scope(shared_db database, bool left) {
    dao::paged_list lists;
    const std::string key = "append";
    std::vector<std::string> model(96, "seed");
    long long length;
    require(lists.push(database, key, model, false, false, length),
        "append scope seed");
    dao::json_key keys;
    require(keys.set_expiry(database, key, deadline, 0), "push scope TTL seed");
    {
        execution_guard admission;
        db::page_lock_scope scope(*database, key);
        dao::paged_list pinned(&scope);
        for (unsigned i = 0; i < 700; ++i) {
            const std::string value(i % 20, static_cast<char>(i));
            const bool pushed = pinned.push(database, key, {value}, left, false, length);
            if (!pushed) std::cerr << "append failed at " << i << ": " << pinned.error() << '\n';
            require(pushed, "append scope push");
            if (left) model.insert(model.begin(), value);
            else model.push_back(value);
            require(length == static_cast<long long>(model.size()),
                "append scope root count");
            if (i % 17 == 0) {
                std::string popped; bool found;
                require(pinned.pop_one(database, key, left, popped, found) &&
                    found && popped == (left ? model.front() : model.back()),
                    "native push then ordinary pop");
                if (left) model.erase(model.begin());
                else model.pop_back();
            }
            if (i % 71 == 0)
                require(contents(pinned, database, key) == model,
                    "append scope full content");
        }
    }
    require(contents(lists, database, key) == model,
        "append scope final content");
    require(keys.expiry_time(database, key, length) && length == deadline,
        "native endpoint push lost TTL");
}

#ifdef HAS_TBB
static void test_bound_root_retry() {
    auto database = db::create_mdb_tbb(4);
    require(database->open(""), "bound root database");
    const std::string key = "bound-root";
    const std::string member("a\0b", 3);
    dao::paged_list lists;
    long long length;
    require(lists.push(database, key, std::vector<std::string>(96, member),
        false, false, length), "bound root seed");
    execution_guard admission;
    db::page_lock_scope scope(*database, key);
    dao::paged_list pinned(&scope);
    require(pinned.push(database, key, {member}, false, false, length) &&
        length == 97, "bound root warm append");

    const db::page_root_callback reject = [](db::batch_entry& entry) {
        require(entry.found && !entry.value.empty(), "bound root snapshot missing");
        entry.value.assign("uncommitted");
        return db::page_root_result::rejected;
    };
    require(scope.transact(*database, key, {}, &reject) == db::batch_status::aborted,
        "bound root callback should abort");
    require(pinned.push(database, key, {member}, false, false, length) &&
        length == 98, "retry used an uncommitted root");

    dao::list_result popped;
    require(pinned.pop(database, key, false, length, popped) &&
        popped.values == std::vector<std::string>(98, member), "bound root erase");
    require(pinned.push(database, key, {member}, false, false, length) &&
        length == 1 && contents(pinned, database, key) ==
            std::vector<std::string>{member}, "bound root recreated with stale pages");
}
#endif

int main() {
#ifdef HAS_TBB
    test_bound_root_retry();
    for (bool left : {false, true}) {
        auto database = db::create_mdb_tbb(4);
        require(database->open(""), "oneTBB append scope open");
        test_native_append_scope(database, left);
    }
#endif
    test_append_observer_fallback(false);
    test_append_observer_fallback(true);
    test_native_append_scope(memory(), false);
    test_native_append_scope(memory(), true);
    test_record_bucket_with_pinned_list();
    test_shard_family_growth_with_pinned_list();
    test_mutation_expiry_index();
    test_callback_lifetime_across_yields();
    test_native_open_metadata();
    test_model();
    test_migration();
    test_transfer();
    test_copy_type_race();
    test_future_page_transfer();
    test_compact_root();
    std::cout << "paged List DAO tests passed\n";
}
