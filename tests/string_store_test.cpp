#include "stdafx.h"
#include "db/string_store.h"
#include "common/redis_string_util.h"
#include "db/mdb/mdb.h"
#include <cstdlib>
#include <iostream>
#include <new>
#include <random>
#include <atomic>
#include <thread>

static thread_local long allocation_budget = -1;
static thread_local size_t allocation_limit = SIZE_MAX;
void* operator new(std::size_t size) {
    if (allocation_budget == 0 || size > allocation_limit)
        throw std::bad_alloc();
    if (allocation_budget > 0) --allocation_budget;
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
static const int64_t deadline = 4102444800123LL;
static void require(bool ok, const char* reason) {
    if (!ok) { std::cerr << reason << '\n'; std::exit(1); }
}
static shared_db memory(bool tbb = false) {
    auto database = tbb ? db::create_mdb_tbb(4) : db::create_mdb(4);
    require(database && database->open(""), "memory open");
    return database;
}
using operation = std::function<bool(string_store&)>;
static db::batch_status run(shared_db database, const std::string& key,
    const operation& callback) {
    return database->transact_pages({key}, [&](page_transaction& tx) {
        string_store store(tx, key);
        return store.open() && callback(store) && store.finish();
    });
}
static std::string contents(shared_db database, const std::string& key) {
    std::string out;
    require(run(database, key, [&](string_store& store) {
        return store.read(out);
    }) == db::batch_status::ok, "String read");
    return out;
}
static void root(shared_db database, const std::string& key,
    const std::string* expected) {
    std::string bytes;
    require(database->get(key, bytes) == (expected != nullptr),
        "String existence");
    if (!expected) return;
    string_root metadata;
    require(string_page_codec::decode_root(bytes, metadata) ==
        string_page_status::ok && metadata.length == expected->size() &&
        bytes.size() <= 1056 && metadata.pages ==
        string_page_codec::pages_for(expected->size()), "String root bounds");
}
static std::string binary(size_t size) {
    std::string value(size, '\0');
    for (size_t i = 0; i < size; ++i) value[i] = static_cast<char>(i % 256);
    return value;
}
static void test_codec() {
    for (size_t size : {0, 1, 1023, 1024, 1025, 16384, 16385, 32768}) {
        string_root input, output;
        input.length = size; input.expires_at_ms = deadline;
        input.pages = string_page_codec::pages_for(size);
        if (!input.pages) input.inline_value = binary(size);
        std::string bytes;
        require(string_page_codec::encode_root(input, bytes) ==
            string_page_status::ok && string_page_codec::decode_root(bytes,
            output) == string_page_status::ok &&
            output.length == input.length && output.pages == input.pages &&
            output.inline_value == input.inline_value &&
            output.expires_at_ms == deadline, "String codec roundtrip");
        for (size_t i = 0; i < bytes.size(); ++i) {
            const auto prefix = bytes.substr(0, i);
            output.length = 17;
            require(string_page_codec::decode_root(prefix, output) !=
                string_page_status::ok && output.length == 17,
                "truncated String root/output changed");
        }
        output.length = 17;
        bytes[3] = 127;
        require(string_page_codec::decode_root(bytes, output) ==
            string_page_status::unsupported && output.length == 17,
            "future String root");
    }
    std::string legacy(32, '\0');
    legacy.replace(0, 4, "PSR\1", 4);
    legacy[7] = 1;
    legacy.replace(8, 8, 8, '\xff');
    legacy[23] = 3;
    legacy.append("a\0b", 3);
    string_root legacy_value;
    require(string_page_codec::decode_root(legacy, legacy_value) ==
        string_page_status::ok && legacy_value.expires_at_ms == -1 &&
        legacy_value.inline_value == std::string("a\0b", 3),
        "legacy inline String root");
    string_root invalid;
    invalid.expires_at_ms = -2;
    std::string unchanged = "keep";
    require(string_page_codec::encode_root(invalid, unchanged) ==
        string_page_status::invalid && unchanged == "keep", "invalid TTL");
    invalid.expires_at_ms = -1;
    invalid.length = string_page_codec::max_bytes + 1;
    require(string_page_codec::encode_root(invalid, unchanged) ==
        string_page_status::invalid && unchanged == "keep", "size overflow");
    invalid.length = string_page_codec::max_bytes;
    invalid.pages = string_page_codec::pages_for(invalid.length);
    require(string_page_codec::encode_root(invalid, unchanged) ==
        string_page_status::ok && unchanged.size() == 32, "maximum root");
    auto bytes = unchanged;
    bytes[7] = 2;
    string_root output;
    require(string_page_codec::decode_root(bytes, output) ==
        string_page_status::unsupported, "future root flags");
    bytes = unchanged; bytes.push_back('x');
    require(string_page_codec::decode_root(bytes, output) ==
        string_page_status::invalid, "trailing paged root bytes");
    bytes = unchanged; bytes[8] = static_cast<char>(0x80);
    require(string_page_codec::decode_root(bytes, output) ==
        string_page_status::invalid, "invalid signed expiry");
}

static void test_model(bool tbb) {
    auto database = memory(tbb);
    std::mt19937 random(731193);
    const std::string key("\0owner\xff", 7);
    std::string expected;
    bool exists = false;
    for (unsigned step = 0; step < 500; ++step) {
        const auto choice = random() % 6;
        const auto value = binary(random() % 50000);
        const uint64_t offset = random() % (expected.size() + 20000);
        require(run(database, key, [&](string_store& store) {
            if (choice == 0) return store.assign(value);
            if (choice == 1) return store.append(value);
            if (choice == 2) return store.write(offset, value);
            if (choice == 3) return store.clear();
            if (choice == 4) return store.set_expiry(deadline);
            std::string out;
            const auto first = static_cast<long long>(offset) - 10000;
            const auto last = first + 20000;
            size_t position, count;
            std::string slice;
            if (string_slice(expected.size(), first, last, position, count))
                slice = expected.substr(position, count);
            return store.range(first, last, out) && out == slice;
        }) == db::batch_status::ok, "String model operation");
        if (choice == 0) { expected = value; exists = true; }
        if (choice == 1) { expected += value; exists = true; }
        if (choice == 2 && !value.empty()) {
            if (expected.size() < offset + value.size())
                expected.resize(static_cast<size_t>(offset) + value.size(), 0);
            expected.replace(static_cast<size_t>(offset), value.size(), value);
            exists = true;
        }
        if (choice == 3) { expected.clear(); exists = false; }
        require(contents(database, key) == expected, "String model bytes");
        root(database, key, exists ? &expected : nullptr);
    }
    require(run(database, "missing", [](string_store& store) {
        return store.write(UINT64_MAX, "") && !store.exists();
    }) == db::batch_status::ok, "empty SETRANGE created missing key");
    require(run(database, "empty", [](string_store& store) {
        return store.append("") && store.exists() && !store.metadata().length;
    }) == db::batch_status::ok, "empty APPEND did not create key");
    const auto inline_value = binary(1024);
    require(run(database, "alias", [&](string_store& store) {
        return store.assign(inline_value) &&
            store.assign(store.metadata().inline_value) &&
            store.append(store.metadata().inline_value);
    }) == db::batch_status::ok &&
        contents(database, "alias") == inline_value + inline_value,
        "inline self-alias/cutover");
}

static void test_concurrency(bool tbb) {
    auto database = memory(tbb);
    std::atomic<bool> failed{false};
    std::vector<std::thread> threads;
    const auto tag = [](unsigned worker, unsigned step) {
        auto text = std::to_string(worker) + ":" + std::to_string(step);
        text.resize(128, 'x'); return text;
    };
    for (unsigned worker = 0; worker < 4; ++worker) {
        threads.emplace_back([&, worker] {
            for (unsigned step = 0; step < 50; ++step) {
                const auto value = tag(worker, step);
                if (run(database, "race", [&](string_store& store) {
                    return store.append(value);
                }) != db::batch_status::ok) failed = true;
            }
        });
    }
    for (auto& thread : threads) thread.join();
    require(!failed, "concurrent String append failed");
    const auto value = contents(database, "race");
    std::set<std::string> expected, actual;
    for (unsigned worker = 0; worker < 4; ++worker)
        for (unsigned step = 0; step < 50; ++step)
            expected.insert(tag(worker, step));
    require(value.size() == 200 * 128, "concurrent String lost length");
    for (size_t offset = 0; offset < value.size(); offset += 128)
        actual.insert(value.substr(offset, 128));
    require(actual == expected, "concurrent String lost/duplicated blocks");
}

class tracking_db : public mdb {
public:
    tracking_db() : mdb(4) {}
    size_t reads = 0, writes = 0, maximum = 0;
protected:
    lookup_status dblookup_page(const std::string& key, uint64_t id,
        std::string& out) override {
        ++reads; return mdb::dblookup_page(key, id, out);
    }
    batch_status dbapply_pages(const std::vector<batch_entry>& roots,
        const std::vector<page_change>& pages) override {
        writes += pages.size();
        for (const auto& page : pages)
            maximum = std::max(maximum, page.value.size());
        return mdb::dbapply_pages(roots, pages);
    }
};
static void test_locality() {
    auto native = std::make_shared<tracking_db>();
    shared_db database = native;
    require(database->open(""), "tracking open");
    const auto value = binary(8 * 1024 * 1024);
    require(run(database, "large", [&](string_store& store) {
        return store.assign(value);
    }) == db::batch_status::ok, "large String seed");
    native->reads = native->writes = 0;
    std::string out;
    allocation_limit = 32768;
    const auto result = run(database, "large", [&](string_store& store) {
        return store.range(16383, 16384, out) && out == value.substr(16383, 2)
            && store.write(5 * 16384 + 1, "x") && store.append("z") &&
            store.set_expiry(deadline);
    });
    allocation_limit = SIZE_MAX;
    require(result == db::batch_status::ok && native->reads == 3 &&
        native->writes == 2 && native->maximum <= 16384,
        "point String operation touched whole value");
    native->reads = native->writes = 0;
    require(run(database, "large", [](string_store& store) {
        return store.set_expiry(-1);
    }) == db::batch_status::ok && !native->reads && !native->writes,
        "String TTL read/wrote data pages");
    require(run(database, "large", [](string_store& store) {
        return store.assign("small");
    }) == db::batch_status::ok, "paged to inline replacement");
    require(database->transact_pages({"large"}, [](page_transaction& tx) {
        std::string bytes;
        return tx.lookup("large", 1, bytes) == db::lookup_status::missing &&
            tx.lookup("large", 513, bytes) == db::lookup_status::missing;
    }) == db::batch_status::ok, "replaced String retained pages");
}

static void test_clone(bool tbb) {
    auto database = memory(tbb), target = memory(tbb);
    const auto input = binary(100000);
    require(run(database, "source", [&](string_store& store) {
        return store.assign(input) && store.set_expiry(deadline);
    }) == db::batch_status::ok, "clone seed");
    bool copied = false;
    require(database->copy_pages_to("source", *target, "source",
        [&](page_transaction& from, page_transaction& to) {
        string_store source(from, "source"), destination(to, "source");
        return source.open() && destination.open() &&
            source.clone_to(destination, copied) && source.finish() &&
            destination.finish();
    }) == db::batch_status::ok && copied &&
        contents(target, "source") == input, "cross-DB String clone");
    require(run(target, "source", [](string_store& store) {
        return store.write(0, "different") &&
            store.metadata().expires_at_ms == deadline;
    }) == db::batch_status::ok && contents(database, "source") == input,
        "String clone aliased source");
    require(database->transact_pages({"source"}, [](page_transaction& tx) {
        string_store source(tx, "source");
        bool copied;
        if (!source.open()) return false;
        source.clone_to(source, copied);
        return true;
    }) != db::batch_status::ok, "same-owner clone accepted");
    require(run(database, "source", [](string_store& store) {
        store.write(UINT64_MAX, "x"); return true;
    }) != db::batch_status::ok && contents(database, "source") == input,
        "ignored overflow changed String");
}

static void test_corruption() {
    auto database = memory();
    const auto input = binary(40000);
    require(run(database, "source", [&](string_store& store) {
        return store.assign(input);
    }) == db::batch_status::ok, "corruption seed");
    std::string root_before;
    require(database->get("source", root_before), "corruption root");
    require(database->transact_pages({"source"}, [](page_transaction& tx) {
        return tx.erase("source", 2);
    }) == db::batch_status::ok, "corrupt page fixture");
    std::string out = "stale";
    require(run(database, "source", [&](string_store& store) {
        return store.range(0, 1, out);
    }) == db::batch_status::ok && out == input.substr(0, 2),
        "range read unrelated damaged page");
    require(run(database, "source", [&](string_store& store) {
        return store.read(out);
    }) != db::batch_status::ok && out.empty(), "missing chunk accepted");
    std::string after;
    require(database->get("source", after) && after == root_before,
        "corrupt read changed root");
    for (const auto& bytes : {std::string("PSR\177future", 10),
        std::string("PSR\1broken", 10)}) {
        require(database->set("bad", bytes), "invalid root seed");
        require(run(database, "bad", [](string_store& store) {
            return store.assign("overwrite");
        }) != db::batch_status::ok && database->get("bad", after) &&
            after == bytes, "bad/future String root overwritten");
    }
    require(run(database, "unfinished", [](string_store& store) {
        return store.append("x");
    }) == db::batch_status::ok, "unfinished seed");
    require(database->transact_pages({"unfinished"}, [](page_transaction& tx) {
        string_store store(tx, "unfinished");
        return store.open() && store.assign("lost");
    }) != db::batch_status::ok && contents(database, "unfinished") == "x",
        "forgotten finish published String");
}

static void test_allocation_rollback(bool tbb, bool clone) {
    const auto initial = binary(40000), replacement = binary(90000);
    bool success = false;
    unsigned failures = 0;
    for (long budget = 0; budget < 800; ++budget) {
        auto database = memory(tbb);
        require(run(database, "source", [&](string_store& store) {
            return store.assign(initial);
        }) == db::batch_status::ok &&
            run(database, "target", [](string_store& store) {
            return store.assign("old");
        }) == db::batch_status::ok, "allocation seed");
        allocation_budget = budget;
        db::batch_status result = db::batch_status::error;
        try {
            result = database->transact_pages({"source", "target"},
                [&](page_transaction& tx) {
                string_store from(tx, "source"), to(tx, "target");
                bool copied;
                if (!from.open() || !to.open()) return false;
                const bool ok = clone ? from.clone_to(to, copied) :
                    (from.write(32000, replacement) && to.assign(replacement));
                return ok && from.finish() && to.finish();
            });
        } catch (const std::bad_alloc&) {}
        allocation_budget = -1;
        if (result == db::batch_status::ok) {
            std::string expected = initial;
            if (!clone) {
                expected.resize(32000 + replacement.size());
                expected.replace(32000, replacement.size(), replacement);
            }
            require(contents(database, "source") == expected &&
                contents(database, "target") ==
                (clone ? initial : replacement), "successful String batch");
            success = true; break;
        }
        ++failures;
        require(contents(database, "source") == initial &&
            contents(database, "target") == "old", "partial String commit");
    }
    require(success && failures > 10, "String allocation sweep");
}

static void disk(const std::string& kind, const std::string& path) {
    const auto factory = [&] {
        auto database = kind == "rdb" ? db::create_rdb() : db::create_wdb();
        require(database && database->open(path.c_str()), "disk String open");
        return database;
    };
    const std::string key("\0binary-owner\xff", 14);
    const auto input = binary(100000);
    {
        auto database = factory();
        require(run(database, key, [&](string_store& store) {
            return store.assign(input) && store.set_expiry(deadline);
        }) == db::batch_status::ok &&
            run(database, "empty", [](string_store& store) {
            return store.assign("");
        }) == db::batch_status::ok, "persistent String seed");
    }
    {
        auto database = factory();
        require(contents(database, key) == input, "String reopen bytes");
        const std::string empty;
        root(database, "empty", &empty);
        require(run(database, key, [](string_store& store) {
            return store.metadata().expires_at_ms == deadline &&
                store.write(16383, "cross-page") && store.append("tail");
        }) == db::batch_status::ok, "persistent String edit");
    }
    auto database = factory();
    auto expected = input;
    expected.replace(16383, 10, "cross-page"); expected += "tail";
    require(contents(database, key) == expected, "String edit reopen");
    std::cout << kind << ": native String pages reopen passed\n";
}

static void test_root_buffer_reuse() {
    for (size_t size : {size_t(0), size_t(3), size_t(1024), size_t(1025)}) {
        string_root root;
        root.length = size; root.pages = string_page_codec::pages_for(size);
        if (!root.pages) root.inline_value = binary(size);
        std::string expected, out = "keep";
        require(string_page_codec::encode_root(root, expected) ==
            string_page_status::ok, "root reuse fixture");
        out.reserve(expected.size());
        allocation_budget = 0;
        const auto result = string_page_codec::encode_root(root, out);
        allocation_budget = -1;
        require(result == string_page_status::ok && out == expected,
            "String root failed to reuse output capacity");
        std::string cold = "keep";
        const bool fits = expected.size() <= cold.capacity();
        allocation_budget = 0;
        const auto failed = string_page_codec::encode_root(root, cold);
        allocation_budget = -1;
        require(fits ? failed == string_page_status::ok && cold == expected :
            failed == string_page_status::out_of_memory && cold == "keep",
            "String root allocation behavior changed output");
        auto tight = root;
        const auto before = tight.inline_value;
        if (tight.inline_value.capacity() < expected.size()) {
            allocation_budget = 0;
            const auto alias_failure = string_page_codec::encode_root(tight,
                tight.inline_value);
            allocation_budget = -1;
            require(alias_failure == string_page_status::out_of_memory &&
                tight.inline_value == before && tight.length == size,
                "aliased root allocation failure changed input/output");
        }
        auto alias = root;
        alias.inline_value.reserve(expected.size());
        allocation_budget = 0;
        const auto aliased = string_page_codec::encode_root(alias,
            alias.inline_value);
        allocation_budget = -1;
        require(aliased == string_page_status::ok &&
            alias.inline_value == expected, "aliased root encode changed data");
        if (!root.pages) {
            auto direct = root.inline_value;
            direct.reserve(expected.size());
            allocation_budget = 0;
            const auto status = string_page_codec::encode_inline(-1, direct,
                direct);
            allocation_budget = -1;
            require(status == string_page_status::ok && direct == expected,
                "inline input alias changed root bytes or allocated");
        }
        root.expires_at_ms = -2;
        require(string_page_codec::encode_root(root, out) ==
            string_page_status::invalid && out == expected,
            "invalid String root changed reused output");
    }
}

static void test_integer_encoding() {
    std::vector<long long> values = {0, 1, -1, LLONG_MIN, LLONG_MIN + 1,
        LLONG_MAX - 1, LLONG_MAX};
    for (long long power = 1;; power *= 10) {
        for (long long delta : {-1, 0, 1}) {
            values.push_back(power + delta);
            values.push_back(-power + delta);
        }
        if (power == 1000000000000000000LL) break;
    }
    std::mt19937_64 random(7189);
    for (unsigned i = 0; i < 5000; ++i)
        values.push_back(static_cast<long long>(random()));
    for (int64_t expiry : {int64_t(-1), int64_t(0), deadline, INT64_MAX}) {
        for (long long value : values) {
            string_root root;
            root.expires_at_ms = expiry;
            root.inline_value = std::to_string(value);
            root.length = root.inline_value.size();
            std::string expected, out;
            require(string_page_codec::encode_root(root, expected) ==
                string_page_status::ok, "reference integer encoding");
            out.reserve(52);
            allocation_budget = 0;
            const auto status = string_page_codec::encode_integer(expiry,
                value, out);
            allocation_budget = -1;
            string_root decoded;
            require(status == string_page_status::ok && out == expected &&
                string_page_codec::decode_root(out, decoded) ==
                    string_page_status::ok &&
                decoded.inline_value == root.inline_value &&
                decoded.expires_at_ms == expiry,
                "integer encoding bytes/TTL/reused capacity");
        }
    }
    std::string out = "unchanged";
    allocation_budget = 0;
    const auto failed = string_page_codec::encode_integer(-1, LLONG_MIN, out);
    allocation_budget = -1;
    require(failed == string_page_status::out_of_memory && out == "unchanged",
        "failed integer encoding changed output");
    require(string_page_codec::encode_integer(-2, 0, out) ==
        string_page_status::invalid && out == "unchanged",
        "invalid integer expiry changed output");
}

int main(int argc, char** argv) {
    test_integer_encoding();
    test_root_buffer_reuse();
    acl::acl_cpp_init();
    if (argc == 4 && std::string(argv[1]) == "--disk") {
        disk(argv[2], argv[3]); return 0;
    }
    require(argc == 1, "invalid arguments");
    test_codec(); test_model(false); test_concurrency(false);
    test_locality(); test_clone(false);
    test_corruption(); test_allocation_rollback(false, false);
    test_allocation_rollback(false, true);
#ifdef HAS_TBB
    test_model(true); test_concurrency(true); test_clone(true);
    test_allocation_rollback(true, false); test_allocation_rollback(true, true);
#endif
    std::cout << "Native String codec, model, locality and rollback passed\n";
}
