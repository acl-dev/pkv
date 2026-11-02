#include "stdafx.h"
#include "common/lcs.h"
#include "dao/paged_string.h"
#include "dao/json/json_string.h"
#include "dao/paged_hash.h"
#include "db/mdb/mdb.h"
#include <climits>
#include <cstdlib>
#include <iostream>
#include <new>
#include <random>
#include <thread>

static thread_local long budget = -1;
static thread_local size_t largest = SIZE_MAX;
void* operator new(std::size_t size) {
    if (!budget || size > largest) throw std::bad_alloc();
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
static std::string model(const std::string& a, const std::string& b) {
    std::vector<std::vector<std::string>> cells(a.size() + 1,
        std::vector<std::string>(b.size() + 1));
    for (size_t i = 1; i <= a.size(); ++i)
        for (size_t j = 1; j <= b.size(); ++j)
            cells[i][j] = a[i - 1] == b[j - 1] ?
                cells[i - 1][j - 1] + a[i - 1] :
                cells[i - 1][j].size() > cells[i][j - 1].size() ?
                    cells[i - 1][j] : cells[i][j - 1];
    return cells.back().back();
}
static void algorithm() {
    std::mt19937 random(918241);
    for (unsigned trial = 0; trial < 800; ++trial) {
        std::string a(random() % 24, '\0'), b(random() % 24, '\0');
        for (auto& ch : a) ch = static_cast<char>(random() % 5);
        for (auto& ch : b) ch = static_cast<char>(random() % 5);
        const auto expected = model(a, b);
        for (unsigned mode = 0; mode < 3; ++mode) {
            lcs_options options;
            options.length_only = mode == 1; options.indexes = mode == 2;
            lcs_result out;
            require(!longest_common_subsequence(a, b, options, out) &&
                out.length == expected.size(), "LCS model length");
            if (!mode) require(out.value == expected, "LCS model bytes");
            if (mode != 2) continue;
            std::string reconstructed;
            uint32_t last_a = UINT32_MAX, last_b = UINT32_MAX;
            for (const auto& match : out.matches) {
                const auto count = match.first_end - match.first_begin + 1;
                require(match.first_end < last_a && match.second_end < last_b
                    && match.second_end - match.second_begin + 1 == count &&
                    a.substr(match.first_begin, count) ==
                    b.substr(match.second_begin, count), "LCS match ranges");
                last_a = match.first_begin; last_b = match.second_begin;
                reconstructed = a.substr(match.first_begin, count) +
                    reconstructed;
            }
            require(reconstructed == expected, "LCS range reconstruction");
        }
    }
    require(lcs_dimensions(UINT64_MAX, 1) &&
        lcs_dimensions(12000, 12000) && !lcs_dimensions(70000, 3),
        "LCS dimension admission");
    std::string a(20000, 'x'), b(20000, 'x');
    lcs_options options;
    options.length_only = true;
    // A quadratic uint32 table would exceed this allocation ceiling.
    // Two-bit traceback is checked on a smaller input below; LEN is linear.
    a.resize(2000); b.resize(2000);
    largest = 16384;
    lcs_result out;
    require(!longest_common_subsequence(a, b, options, out) &&
        out.length == 2000, "LCS LEN row allocation bound");
    options.length_only = false;
    a.resize(200); b.resize(200);
    require(!longest_common_subsequence(a, b, options, out) &&
        out.value == a, "LCS compact traceback allocation bound");
    largest = SIZE_MAX;
}
static void snapshots(shared_db database, bool legacy) {
    dao::paged_string data;
    dao::json_string old;
    dao::set_options set;
    set.expires_at_ms = 4102444800123LL;
    dao::set_result stored;
    const std::string a(70000, 'x'), b("x\0x", 3);
    require((legacy ? old.set(database, "old", a, set, stored) :
        data.set(database, "old", a, set, stored)) &&
        data.set(database, "new", b, set, stored), "LCS snapshot seed");
    std::string before, after;
    require(database->get("old", before), "LCS legacy bytes");
    std::vector<watch_token> watched;
    require(database->watch({"old", "new"}, watched) == db::batch_status::ok,
        "LCS WATCH setup");
    lcs_options options;
    lcs_result out;
    const char* error;
    require(data.lcs(database, "old", "new", {}, options, out, error) &&
        out.value == "xx", "LCS legacy/native sources");
    require(database->get("old", after) && after == before &&
        watched[0]->valid(0) && watched[1]->valid(0),
        "LCS changed legacy bytes or WATCH");
    for (const std::string word : {"LEN", "IDX", "WITHMATCHLEN"}) {
        std::vector<const std::string*> arguments = {&word};
        budget = LONG_MAX;
        require(data.lcs(database, "old", "new", arguments, options, out,
            error), "LCS budget control");
        const auto allocations = LONG_MAX - budget;
        budget = -1;
        for (long position = 0; position <= allocations; ++position) {
            budget = position;
            try {
                if (data.lcs(database, "old", "new", arguments, options,
                    out, error))
                    require(out.length == 2, "LCS fault-budget result");
            } catch (const std::bad_alloc&) {}
            budget = -1;
            require(database->get("old", after) && after == before &&
                watched[0]->valid(0) && watched[1]->valid(0),
                "LCS allocation failure changed sources");
        }
        std::cout << "LCS " << (legacy ? "legacy " : "native ") << word
            << " allocation positions: " << allocations + 1 << '\n';
    }
}
static void coherent(shared_db database) {
    dao::paged_string setup;
    bool applied;
    require(setup.mset(database, {{"a", "abc"}, {"b", "abc"}}, false,
        applied), "LCS coherence seed");
    std::thread writer([&] {
        dao::paged_string data;
        for (unsigned i = 0; i < 150; ++i) {
            const std::string text = i % 2 ? "abc" : "xyz";
            bool done;
            require(data.mset(database, {{"a", text}, {"b", text}}, false,
                done), "LCS concurrent MSET");
        }
    });
    for (unsigned i = 0; i < 150; ++i) {
        dao::paged_string data;
        lcs_options options;
        lcs_result out;
        const char* error;
        require(data.lcs(database, "a", "b", {}, options, out, error) &&
            out.length == 3, "LCS read torn pair");
    }
    writer.join();
}
class failing_pages : public mdb {
public:
    failing_pages() : mdb(4) {}
    bool reject = false;
    size_t reads = 0;
protected:
    lookup_status dblookup_page(const std::string& owner, uint64_t page,
        std::string& out) override {
        ++reads;
        if (reject) return lookup_status::error;
        return mdb::dblookup_page(owner, page, out);
    }
};
static void read_failures() {
    auto source = std::make_shared<failing_pages>();
    shared_db database = source;
    require(database->open(""), "LCS page failure database");
    dao::paged_string data;
    require(data.set(database, "large", std::string(70000, 'x')) &&
        data.set(database, "small", "x"), "LCS page failure seed");
    lcs_options options;
    lcs_result out;
    const char* error;
    source->reads = 0;
    require(!data.lcs(database, "large", "large", {}, options, out, error)
        && error && !source->reads, "LCS oversized table read payload");
    source->reject = true;
    require(!data.lcs(database, "large", "small", {}, options, out, error)
        && !error && data.status() == dao::dao_base::read_status::storage_error
        && out.value.empty() && out.matches.empty(), "LCS lost page error");
    source->reject = false;
    require(data.lcs(database, "large", "small", {}, options, out, error)
        && out.value == "x", "LCS failed read damaged sources");
}
int main() {
    acl::acl_cpp_init(); algorithm(); read_failures();
    for (bool tbb : {false, true}) {
#ifndef HAS_TBB
        if (tbb) continue;
#endif
        auto database = tbb ? db::create_mdb_tbb(4) : db::create_mdb(4);
        require(database->open(""), "LCS open memory");
        snapshots(database, true); snapshots(database, false);
        coherent(database);
    }
    std::cout << "LCS algorithm and snapshot checks passed\n";
}
