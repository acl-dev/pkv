#include "stdafx.h"
#include "dao/paged_list.h"
#include "dao/paged_string.h"
#include "dao/json/json_list.h"
#include "dao/json/json_key.h"
#include "dao/json/json_string.h"
#include "coder/json_record_codec.h"
#include "db/mdb/mdb.h"
#include <cstdlib>
#include <iostream>
#include <random>
#include <thread>
#include <atomic>

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
    json_record_codec codec;
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

static void test_future_page_transfer() {
    shared_db database = memory();
    dao::paged_list pages;
    dao::json_key keys;
    long long length;
    require(pages.push(database, "future", {"v"}, false, false, length),
        "future page seed");
    require(database->transact_pages({"future"}, [](page_transaction& tx) {
        std::string bytes;
        if (tx.lookup("future", 1, bytes) != db::lookup_status::found)
            return false;
        bytes[3] = 2; // Physical pages currently have version 1 only.
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

int main() {
    test_model();
    test_migration();
    test_transfer();
    test_copy_type_race();
    test_future_page_transfer();
    std::cout << "paged List DAO tests passed\n";
}
