#include "stdafx.h"
#include "coder/string_page.h"
#include "dao/paged_string.h"
#include "dao/json/json_string.h"
#include "db/mdb/mdb.h"
#include "db/mdb/mdb_tbb.h"
#include "db/page_workspace.h"
#include <atomic>
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
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
#endif
using namespace pkv;
using status = db::inline_status;
static void require(bool ok, const char* reason) {
    if (!ok) { std::cerr << reason << '\n'; std::exit(1); }
}
static std::string encoded(const std::string& value, int64_t expiry = -1) {
    string_root root;
    root.length = value.size(); root.inline_value = value;
    root.expires_at_ms = expiry;
    std::string bytes;
    require(string_page_codec::encode_root(root, bytes) ==
        string_page_status::ok, "inline encoding");
    return bytes;
}
static void basic(shared_db database) {
    dao::paged_string data;
    std::string out("stale");
    require(database->read_inline_string("missing", out) == status::missing &&
        out.empty(), "missing inline output");
    for (size_t size : {size_t(0), size_t(64), size_t(1024)}) {
        const std::string value(size, '\0');
        require(data.set(database, "key", value), "inline seed");
        std::vector<watch_token> watched;
        require(database->watch({"key"}, watched) == db::batch_status::ok,
            "inline watch");
        out.reserve(1024);
        budget = 0;
        const bool ok = data.get(database, "key", out);
        budget = -1;
        require(ok && out == value && data.status() ==
            dao::dao_base::read_status::ok, "inline GET allocated or failed");
        require(watched[0]->valid(unix_time_ms()), "GET invalidated WATCH");
    }
    std::string empty;
    budget = 0;
    const bool ok = data.get(database, "key", empty);
    budget = -1;
    require(!ok && empty.empty() && data.status() ==
        dao::dao_base::read_status::storage_error, "GET allocation failure");
    require(data.get(database, "key", out) && out.size() == 1024,
        "GET allocation failure changed stored value");
    require(database->set("future", encoded("live", 4102444800123LL)) &&
        database->read_inline_string("future", out) == status::found &&
        out == "live", "inline TTL read");
    require(database->set("expired", encoded("gone", 1)) &&
        database->read_inline_string("expired", out) == status::fallback &&
        out.empty(), "expired inline did not fall back");
    require(!data.get(database, "expired", out) && out.empty() &&
        data.status() == dao::dao_base::read_status::missing,
        "expired inline GET");
    require(database->read_inline_string("expired", out) == status::missing,
        "expired inline not reclaimed");
}
static void fallbacks(shared_db database) {
    dao::paged_string data;
    std::string out;
    const std::string large(32769, 'L');
    require(data.set(database, "large", large) &&
        database->read_inline_string("large", out) == status::fallback &&
        data.get(database, "large", out) && out == large, "paged fallback");
    dao::json_string legacy;
    require(legacy.set(database, "legacy", "old") &&
        database->read_inline_string("legacy", out) == status::fallback &&
        out.empty() && data.get(database, "legacy", out) && out == "old" &&
        database->read_inline_string("legacy", out) == status::found,
        "legacy import fallback");
    for (auto bytes : {std::string("garbage"), std::string("PSR\2", 4),
        encoded("broken").substr(0, 34), encoded("flags")}) {
        if (bytes.size() == 37) bytes[4] = 2;
        require(database->set("bad", bytes) &&
            database->read_inline_string("bad", out) == status::fallback &&
            out.empty() && !data.get(database, "bad", out) && out.empty() &&
            data.status() != dao::dao_base::read_status::missing,
            "invalid/unsupported root bypassed validation");
    }
    require(data.set(database, "workspace", "before"), "workspace seed");
    {
        shared_db pending = std::make_shared<page_workspace>(database);
        auto workspace = std::static_pointer_cast<page_workspace>(pending);
        require(pending->read_inline_string("workspace", out) ==
            status::fallback && data.set(pending, "workspace", "after") &&
            data.get(pending, "workspace", out) && out == "after",
            "workspace read-your-writes");
        require(workspace->commit() == db::batch_status::ok,
            "workspace inline commit");
    }
    require(data.get(database, "workspace", out) && out == "after",
        "workspace inline publication");
}
static bool complete(const std::string& value) {
    if (value.empty()) return true;
    if (value.size() != 64 && value.size() != 1024 &&
        value.size() != 1025 && value.size() != 32769) return false;
    return value.find_first_not_of(value[0]) == std::string::npos &&
        value[0] >= 'a' && value[0] <= 'z';
}
static void concurrent(shared_db database) {
    std::atomic<bool> start(false), done(false), failed(false);
    std::atomic<size_t> reads(0);
    std::vector<std::thread> readers;
    for (int i = 0; i < 8; ++i) readers.emplace_back([&] {
        dao::paged_string data;
        std::string out;
        while (!start.load()) std::this_thread::yield();
        do {
            const bool ok = data.get(database, "racing", out);
            if ((ok && !complete(out)) || (!ok && (!out.empty() ||
                data.status() != dao::dao_base::read_status::missing)))
                failed.store(true);
            ++reads;
        } while (!done.load());
    });
    start.store(true);
    dao::paged_string data;
    for (int i = 0; i < 1200; ++i) {
        const size_t sizes[] = {0, 64, 1024, 1025, 32769};
        const std::string value(sizes[i % 5], char('a' + i % 26));
        if (!data.set(database, "racing", value)) failed.store(true);
        if (i % 3 == 0 && !database->replica_set("racing", encoded("")))
            failed.store(true);
        if (i % 7 == 0 && !database->del("racing")) failed.store(true);
    }
    done.store(true);
    for (auto& reader : readers) reader.join();
    require(reads.load() >= 8 && !failed.load(),
        "concurrent inline/paged/replica/delete read was torn");
}
template<class Backend> class tracked : public Backend {
public:
    tracked() : Backend(4) {}
    std::atomic<size_t> roots{0}, pages{0};
protected:
    db::lookup_status dblookup(const std::string& key,
        std::string& out) override {
        ++roots; return Backend::dblookup(key, out);
    }
    db::lookup_status dblookup_page(const std::string& key, uint64_t id,
        std::string& out) override {
        ++pages; return Backend::dblookup_page(key, id, out);
    }
};
template<class Backend> static void run() {
    auto source = std::make_shared<tracked<Backend>>();
    shared_db database = source;
    require(database->open(""), "inline database open");
    dao::paged_string data;
    std::string out;
    require(data.set(database, "direct", "small"), "direct seed");
    source->roots = 0; source->pages = 0;
    require(data.get(database, "direct", out) && out == "small" &&
        !source->roots.load() && !source->pages.load(),
        "GET constructed page transaction");
    basic(database); fallbacks(database);
    concurrent(database);
}
int main() {
    run<mdb>();
#ifdef HAS_TBB
    run<mdb_tbb>();
#endif
    std::cout << "Inline String bounded reads, zero-allocation, fallback, "
        "workspace and concurrent replacement passed\n";
}
