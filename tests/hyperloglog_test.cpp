#include "stdafx.h"
#include "common/redis_hll.h"
#include "common/command_keys.h"
#include "dao/paged_string.h"
#include "dao/json/json_string.h"
#include "dao/json/json_key.h"
#include "db/page_transaction.h"
#include <cstdlib>
#include <iostream>
#include <random>

using namespace pkv;
static void require(bool ok, const char* reason) {
    if (!ok) { std::cerr << reason << '\n'; std::exit(1); }
}
static void codec() {
    auto value = hll::empty();
    require(value.size() == 18 && hll::header(value, value.size()),
        "empty sparse HLL");
    hll::registers input{}, output{};
    require(hll::merge(value, output) && hll::count(output) == 0,
        "empty HLL cardinality");
    require(hll::dense(value) && value.size() == hll::dense_size,
        "empty dense conversion");
    std::mt19937 random(7107);
    for (size_t i = 0; i < hll::registers_size; ++i) {
        input[i] = random() % 64; hll::put(value, i, input[i]);
    }
    for (size_t i = 0; i < hll::registers_size; ++i)
        require(hll::get(value, i) == input[i], "dense bit packing");
    require(hll::merge(value, output) && input == output,
        "dense register merge");
    hll::registers maximum{};
    for (size_t i = 0; i < hll::registers_size; ++i) {
        output[i] = static_cast<uint8_t>(random() % 64);
        maximum[i] = std::max(input[i], output[i]);
    }
    require(hll::merge(value, output) && output == maximum,
        "dense merge did not preserve register maxima");
    value[5] = '\x71';
    value[7] = '\x53';
    hll::cache(value, 889);
    auto packed = value.substr(0, hll::header_size);
    packed[4] = 1;
    hll::pack_dense(packed, input);
    require(packed == value, "dense packed bytes differ from scalar encoding");
    value.pop_back();
    require(!hll::merge(value, output) && output == maximum,
        "truncated dense merge changed registers");
    value.resize(hll::dense_size);
    for (unsigned shape = 0; shape < 4; ++shape) {
        for (size_t i = 0; i < input.size(); ++i) {
            input[i] = shape == 0 ? 0 : shape == 1 ? 7 :
                shape == 2 ? (i % 4 ? 0 : random() % 17) : random() % 52;
            hll::put(value, i, input[i]);
        }
        uint64_t scalar;
        require(hll::count_encoded(value, scalar) && hll::count(input) == scalar,
            "raw histogram cardinality differs from dense histogram");
    }
    value = hll::empty();
    for (size_t i = 0; i < 20000; ++i) {
        bool changed;
        require(hll::add(value, std::to_string(i), changed), "HLL update");
    }
    require(!value[4], "sparse representation did not promote");
    output.fill(0);
    require(hll::merge(value, output), "updated register merge");
    const auto estimate = hll::count(output);
    require(estimate > 19000 && estimate < 21000, "cardinality estimate");
    hll::invalidate(value);
    uint64_t cached;
    require(!hll::cached(value, cached), "cache invalidation");
    hll::cache(value, estimate);
    require(hll::cached(value, cached) && cached == estimate, "HLL cache");
    for (auto body : {std::string(), std::string("\x40", 1),
        std::string("\x7f\xff\0", 3)}) {
        value = hll::empty().substr(0, 16) + body;
        output.fill(0);
        require(!hll::merge(value, output), "corrupt sparse body accepted");
    }
}
static void legacy(shared_db& database) {
    dao::json_string json;
    dao::paged_string data;
    auto value = hll::empty();
    const std::string first("first"), second("second");
    bool changed;
    require(hll::add(value, first, changed), "legacy HLL value");
    hll::cache(value, 1);
    require(json.set(database, "legacy", value), "legacy HLL seed");
    std::string before, after;
    require(database->get("legacy", before), "legacy snapshot");
    long long count;
    require(data.pfcount(database, {"legacy", "missing"}, count) &&
        count == 1 && database->get("legacy", after) && before == after,
        "union PFCOUNT rewrote legacy source");
    std::vector<watch_token> watched;
    require(database->watch({"legacy"}, watched) == db::batch_status::ok,
        "legacy HLL watch");
    require(data.pfcount(database, {"legacy"}, count) && count == 1 &&
        watched[0]->valid(unix_time_ms()), "HLL import invalidated WATCH");
    require(data.pfadd(database, "legacy", {&first}, count) && !count &&
        watched[0]->valid(unix_time_ms()), "legacy duplicate HLL mutation");
    require(data.pfadd(database, "legacy", {&second}, count) && count == 1 &&
        !watched[0]->valid(unix_time_ms()), "legacy HLL update lost WATCH");
}
static void native(bool tbb) {
    auto database = tbb ? db::create_mdb_tbb(16) : db::create_mdb(16);
    require(database && database->open(""), "HLL database open");
    legacy(database);
    dao::paged_string data;
    std::string first("first"), second("second"), third("third");
    long long result;
    require(data.pfcount(database, {"missing"}, result) && !result,
        "missing HLL count");
    require(data.pfadd(database, "a", {&first, &second}, result) && result == 1,
        "native HLL add");
    require(data.pfadd(database, "a", {&first}, result) && result == 0,
        "duplicate HLL element");
    require(data.pfadd(database, "b", {&third}, result), "second HLL add");
    require(data.pfmerge(database, {"out", "a", "b"}), "native HLL merge");
    require(data.pfcount(database, {"out"}, result) && result == 3,
        "merged cardinality");
    require(data.pfcount(database, {"a", "b"}, result) && result == 3,
        "union cardinality");
    std::string before, after;
    require(data.get(database, "out", before), "snapshot merge destination");
    require(data.set(database, "bad", "not HLL"), "bad source seed");
    require(!data.pfmerge(database, {"out", "a", "bad"}) &&
        data.get(database, "out", after) && before == after,
        "failed merge changed destination");
    auto dense = hll::empty();
    require(hll::dense(dense), "empty dense source");
    hll::put(dense, hll::registers_size - 1, 51);
    require(data.set(database, "dense", dense), "dense merge source");
    before[5] = '\x42';
    require(data.set(database, "out", before), "reserved HLL header seed");
    const long long deadline = 4102444800123LL;
    dao::json_key key_data;
    require(key_data.set_expiry(database, "out", deadline, 0), "merge TTL seed");
    std::vector<watch_token> tokens;
    require(database->watch({"out"}, tokens) == db::batch_status::ok &&
        tokens.size() == 1, "dense merge WATCH");
    hll::registers maximum{};
    require(hll::merge(before, maximum) && hll::merge(dense, maximum),
        "dense merge reference inputs");
    auto expected = before.substr(0, hll::header_size);
    expected.resize(hll::dense_size, '\0');
    expected[4] = 0;
    for (size_t i = 0; i < maximum.size(); ++i)
        hll::put(expected, i, maximum[i]);
    hll::invalidate(expected);
    string_root_header header;
    std::string root;
    require(data.pfmerge(database, {"out", "dense", "out"}) &&
        data.get(database, "out", after) && after == expected &&
        data.get(database, "dense", after) && after == dense &&
        database->get("out", root) &&
        string_page_codec::decode_header(root, header) == string_page_status::ok &&
        header.expires_at_ms == deadline && !tokens[0]->valid(unix_time_ms()),
        "dense merge bytes, source, TTL or WATCH");
    require(database->transact_pages({"out"}, [](page_transaction& tx) {
        for (const auto& root : tx.roots())
            if (root.value.size() > page_transaction::max_value_bytes)
                return false;
        return true;
    }) == db::batch_status::ok, "unbounded HLL root");
}
static void keys() {
    std::string name("PFCOUNT"), a("a"), b("b"), info("INFO");
    std::vector<const std::string*> args{&name, &a, &b};
    require(command_keys(args) && args.size() == 2 && args[0] == &a &&
        args[1] == &b, "PFCOUNT lost source key");
    name = "PFMERGE"; args = {&name, &a, &b};
    require(command_keys(args) && args.size() == 2, "PFMERGE source keys");
    args = {&info, &a};
    require(command_keys(args) && args.empty(), "INFO section became a key");
}
int main() {
    codec(); keys(); native(false);
#ifdef HAS_TBB
    native(true);
#endif
    std::cout << "HyperLogLog codec, native operations and key discovery "
        "passed\n";
}
