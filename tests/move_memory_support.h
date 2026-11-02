#pragma once
#include "move_recovery_types.h"
#include "db/move_memory.h"
#include "db/mdb/mdb.h"
#include "db/mdb/mdb_tbb.h"
#include "db/page_workspace.h"
#include "db/db_watcher.h"
#include <cstdlib>
#include <new>
#include <sys/resource.h>

static thread_local long family_budget = -1;
static thread_local bool family_measuring = false;
static thread_local size_t family_allocations = 0, family_bytes = 0;
void* operator new(std::size_t size) {
    if (!family_budget) throw std::bad_alloc();
    if (family_budget > 0) --family_budget;
    if (family_measuring) { ++family_allocations; family_bytes += size; }
    if (void* out = std::malloc(size ? size : 1)) return out;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* value) noexcept { std::free(value); }
void operator delete[](void* value) noexcept { std::free(value); }
#if __cplusplus >= 201402L
void operator delete(void* value, std::size_t) noexcept { std::free(value); }
void operator delete[](void* value, std::size_t) noexcept { std::free(value); }
#endif
static const int64_t family_deadline = 4102444800000LL;
static const std::string family_key("family\0key", 10);
struct family_control {
    unsigned lookups = 0, visits = 0, page_reads = 0, page_writes = 0;
    unsigned scans = 0;
    bool fail_prepare = false, fail_read = false;
    virtual ~family_control() = default;
    virtual db::batch_status visit(const std::string& owner,
        const db::page_visitor& callback) = 0;
    void reset() { lookups = visits = page_reads = page_writes = scans = 0; }
};
template<class Base> struct family_probe : Base, family_control {
    explicit family_probe(size_t count) : Base(count) {}
    db::batch_status visit(const std::string& owner,
        const db::page_visitor& callback) override {
        return Base::dbvisit_pages(owner, callback);
    }
protected:
    db::lookup_status dblookup_bounded_root(const std::string& key,
        std::string& out) override {
        ++lookups;
        return fail_read ? db::lookup_status::error :
            Base::dblookup_bounded_root(key, out);
    }
    db::lookup_status dblookup_page(const std::string& owner, uint64_t id,
        std::string& out) override {
        ++page_reads; return Base::dblookup_page(owner, id, out);
    }
    db::batch_status dbvisit_pages(const std::string& owner,
        const db::page_visitor& callback) override {
        ++visits; return Base::dbvisit_pages(owner, callback);
    }
    db::scan_status dbscan_keys(unsigned long long cursor, size_t count,
        std::vector<std::string>& keys, unsigned long long& next) override {
        ++scans; return Base::dbscan_keys(cursor, count, keys, next);
    }
    db::batch_status dbprepare_pages(const std::vector<db::batch_entry>& roots,
        const std::vector<page_change>& pages,
        db::prepare_callback& ready) override {
        page_writes += static_cast<unsigned>(pages.size());
        if (fail_prepare) return db::batch_status::error;
        return Base::dbprepare_pages(roots, pages, ready);
    }
};
static shared_db memory_database(bool tbb, size_t count) {
#ifdef HAS_TBB
    if (tbb) return std::make_shared<family_probe<mdb_tbb>>(count);
#else
    require(!tbb, "oneTBB unavailable");
#endif
    return std::make_shared<family_probe<mdb>>(count);
}
static family_control& control(shared_db database) {
    auto* out = dynamic_cast<family_control*>(database.get());
    require(out, "family probe control"); return *out;
}
struct family_page {
    uintptr_t address;
    size_t bytes;
    uLong crc;
    bool operator==(const family_page& other) const {
        return address == other.address && bytes == other.bytes &&
            crc == other.crc;
    }
};
struct memory_image {
    bool found = false;
    std::string root;
    std::map<uint64_t, family_page> pages;
    bool operator==(const memory_image& other) const {
        return found == other.found && root == other.root &&
            pages == other.pages;
    }
};
static memory_image image(shared_db database, const std::string& key) {
    execution_guard admission(true);
    memory_image out;
    require(database->transact_pages({key}, [&](page_transaction& tx) {
        const auto& root = *tx.roots().find(key);
        out.found = root.found; out.root = root.value;
        return control(database).visit(key,
            [&](uint64_t id, const std::string& bytes) {
            out.pages.emplace(id, family_page{
                reinterpret_cast<uintptr_t>(bytes.data()), bytes.size(),
                ::crc32(0, reinterpret_cast<const Bytef*>(bytes.data()),
                    static_cast<uInt>(bytes.size()))});
            return true;
        }) == db::batch_status::ok;
    }) == db::batch_status::ok, "memory family image");
    return out;
}
struct family_events : db_watcher {
    unsigned calls = 0;
    bool supports_pages() const override { return true; }
    bool on_pages_database(unsigned, const std::vector<db::batch_entry>&,
        const std::vector<page_change>&) override { ++calls; return true; }
    bool on_set(const std::string&, const std::string&, bool) override {
        ++calls; return true;
    }
    bool on_get(const std::string&, const std::string&, bool) override {
        return true;
    }
    bool on_del(const std::string&, bool) override { ++calls; return true; }
};
static void seed_memory(shared_db database, const std::string& key,
    size_t count, int64_t expiry) {
    if (count <= 256) { seed(database, key, count, expiry); return; }
    for (size_t first = 1; first <= count; first += 256) {
        const auto last = std::min(count, first + 255);
        require(database->transact_pages({key}, [&](page_transaction& tx) {
            auto& root = *tx.roots().find(key);
            if (first == 1 && !tx.reset_pages(key)) return false;
            root.value = encoded_root(last, expiry);
            root.action = db::update_action::write;
            for (size_t id = first; id <= last; ++id)
                if (!tx.put(key, id, page_bytes(id))) return false;
            return true;
        }) == db::batch_status::ok, "chunked memory family seed");
    }
}
struct memory_fixture {
    family_events events;
    db_watchers source_events{0}, target_events{1};
    shared_db from, to;
    std::unique_ptr<database_pool> pool;
    memory_fixture(bool tbb, bool source_first, bool existing,
        size_t pages = 3) {
        from = memory_database(tbb, 4); to = memory_database(tbb, 13);
        if (std::less<db*>()(from.get(), to.get()) != source_first)
            from.swap(to);
        source_events.add_watcher(&events); target_events.add_watcher(&events);
        require(from->open("", &source_events) && to->open("", &target_events),
            "memory family open");
        pool.reset(new database_pool(from, 2, [this](size_t id) {
            return id == 1 ? to : shared_db();
        }));
        seed_memory(from, family_key, pages, family_deadline);
        if (existing) seed(to, family_key, 5, 1);
        events.calls = 0; control(from).reset(); control(to).reset();
    }
};
static move_memory::callbacks memory_actions(unsigned& completed) {
    move_memory::callbacks out;
    out.prepare = [](const move_memory::roots& view) {
        if (!view.source_found) return move_memory::choice::skip;
        record_header header;
        const auto codec = default_record_codec();
        if (codec->stored_header(view.source, header) != codec_status::ok)
            return move_memory::choice::error;
        if (header.expires_at_ms >= 0 && header.expires_at_ms <= unix_time_ms())
            return move_memory::choice::skip;
        if (view.target_found) {
            if (default_record_codec()->stored_header(view.target, header) !=
                codec_status::ok) return move_memory::choice::error;
            if (header.expires_at_ms < 0 ||
                header.expires_at_ms > unix_time_ms())
                return move_memory::choice::skip;
        }
        return move_memory::choice::proceed;
    };
    out.complete = [&completed] { ++completed; };
    return out;
}
static void no_page_copy(memory_fixture& files) {
    for (auto database : {files.from, files.to}) {
        const auto& calls = control(database);
        require(calls.lookups <= 1 && !calls.page_reads && !calls.page_writes &&
            !calls.visits, "ownership transfer visited/copied payload pages");
    }
    require(!files.events.calls, "incomplete page observation emitted");
}
