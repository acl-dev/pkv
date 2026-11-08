#include "stdafx.h"
#include "rpc/page_sequence.h"
#include "db/mdb/mdb.h"
#include "coder/string_page.h"
#include "coder/record_codec.h"
#include <cstdlib>
#include <iostream>
#include <new>
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
static void require(bool ok, const char* message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}
static std::string encoded(const std::string& value) {
    string_root root; root.length = value.size(); root.inline_value = value;
    std::string bytes;
    require(string_page_codec::encode_root(root, bytes) ==
        string_page_status::ok, "String encode");
    return bytes;
}
static std::shared_ptr<const page_batch> batch(const std::string& key,
    action operation, const std::string& value = "", bool reset = false,
    const std::vector<page_change>& pages = {}, bool maintenance = false,
    unsigned database = 0) {
    std::vector<db::batch_entry> roots;
    roots.emplace_back(key, true, value);
    roots.back().action = operation;
    roots.back().clear_pages = reset;
    roots.back().maintenance_only = maintenance;
    auto out = page_batch::capture(database, roots, pages);
    require(out != nullptr, "batch capture");
    return out;
}
static void add(page_sequence& sequence,
    const std::shared_ptr<const page_batch>& value) {
    require(sequence.append(value), "sequence append");
}
static std::string root(shared_db database, const std::string& key) {
    std::string value;
    require(database->get(key, value), "root missing"); return value;
}
static bool page(shared_db database, const std::string& key, uint64_t id,
    std::string& out) {
    bool found = false;
    require(database->transact_pages({key}, [&](page_transaction& tx) {
        const auto lookup = tx.lookup(key, id, out);
        found = lookup == db::lookup_status::found;
        return lookup != db::lookup_status::error;
    }) == status::ok, "page lookup");
    return found;
}
static watch_token watch(shared_db database, const std::string& key) {
    std::vector<watch_token> tokens;
    require(database->watch({key}, tokens) == status::ok, "WATCH");
    return tokens.front();
}
static void seed(shared_db database, const std::string& key) {
    require(batch(key, action::write, encoded("old"), true,
        {{key, 1, "old-1", action::write},
         {key, 99, "old-99", action::write}})->apply(*database) == status::ok,
        "sequence seed");
}
static void replacement(shared_db database) {
    const std::string key("seq\0key", 7);
    seed(database, key);
    page_sequence sequence;
    add(sequence, batch(key, action::write, encoded("first"), false,
        {{key, 1, "first-1", action::write},
         {key, 2, "first-2", action::write}}));
    add(sequence, batch(key, action::write, encoded("replacement"), true,
        {{key, 4, "new-4", action::write}}));
    add(sequence, batch(key, action::keep, "", false,
        {{key, 5, "new-5", action::write}}));
    require(sequence.apply(*database) == status::ok &&
        root(database, key) == encoded("replacement"), "last metadata wins");
    std::string out;
    for (uint64_t id : {1, 2, 99})
        require(!page(database, key, id, out), "reset resurrected stale page");
    require(page(database, key, 4, out) && out == "new-4" &&
        page(database, key, 5, out) && out == "new-5", "post-reset pages");
    sequence.clear();
    add(sequence, batch(key, action::erase));
    add(sequence, batch(key, action::write, encoded("reborn"), false,
        {{key, 8, "reborn-8", action::write}}));
    require(sequence.apply(*database) == status::ok &&
        root(database, key) == encoded("reborn") &&
        !page(database, key, 4, out) && !page(database, key, 5, out) &&
        page(database, key, 8, out) && out == "reborn-8",
        "erase/recreate preserved an old page family");
}
static void independent(shared_db database) {
    const std::string a = "independent-a", b = "independent-b";
    seed(database, a); seed(database, b);
    auto first = watch(database, a), second = watch(database, b);
    auto created = watch(database, "independent-created");
    auto missing = watch(database, "independent-missing");
    page_sequence sequence;
    add(sequence, batch(a, action::write, encoded("new"), true,
        {{a, 4, "new-4", action::write}}));
    add(sequence, batch(b, action::keep, "", false,
        {{b, 1, "updated-1", action::write}}));
    add(sequence, batch("independent-created", action::write, encoded("x"),
        true, {}, true));
    add(sequence, batch("independent-missing", action::erase, "", false,
        {{"independent-missing", 1, "", action::erase}}));
    require(sequence.apply(*database) == status::ok &&
        !first->valid(unix_time_ms()) && !second->valid(unix_time_ms()) &&
        !created->valid(unix_time_ms()) && missing->valid(unix_time_ms()),
        "disjoint replay WATCH");
    std::string out;
    require(root(database, a) == encoded("new") &&
        !page(database, a, 99, out) && page(database, a, 4, out) &&
        out == "new-4" && page(database, b, 1, out) && out == "updated-1" &&
        page(database, b, 99, out) && out == "old-99",
        "disjoint replay page/reset mismatch");
    page_sequence invalid;
    auto unchanged = watch(database, "independent-prefix");
    add(invalid, batch("independent-prefix", action::write, encoded("x")));
    add(invalid, batch("independent-no-base", action::keep, "", false,
        {{"independent-no-base", 1, "invalid", action::write}}));
    require(invalid.apply(*database) == status::aborted &&
        !database->get("independent-prefix", out) &&
        unchanged->valid(unix_time_ms()), "disjoint replay committed prefix");
}
static void prefix_validation(shared_db database) {
    require(database->replica_erase("missing") &&
        database->set("untouched", encoded("original")), "prefix seed");
    page_sequence sequence;
    add(sequence, batch("untouched", action::write, encoded("must-abort")));
    add(sequence, batch("missing", action::keep, "", false,
        {{"missing", 1, "invalid-prefix", action::write}}));
    add(sequence, batch("missing", action::write, encoded("later-repair")));
    require(sequence.apply(*database) == status::aborted &&
        root(database, "untouched") == encoded("original"),
        "flattening hid an invalid missing-base prefix");
    sequence.clear();
    add(sequence, batch("untouched", action::erase));
    add(sequence, batch("untouched", action::keep, "", false,
        {{"untouched", 1, "invalid-after-delete", action::write}}));
    add(sequence, batch("untouched", action::write, encoded("repair")));
    require(sequence.apply(*database) == status::aborted &&
        root(database, "untouched") == encoded("original"),
        "flattening hid an invalid deleted-base prefix");
}
static void watch_history(shared_db database) {
    require(database->replica_erase("ephemeral"), "ephemeral reset");
    auto missing = watch(database, "ephemeral");
    page_sequence sequence;
    add(sequence, batch("ephemeral", action::write, encoded("brief")));
    add(sequence, batch("ephemeral", action::erase));
    require(sequence.apply(*database) == status::ok &&
        !missing->valid(unix_time_ms()), "net-delete lost creation history");
    missing = watch(database, "ephemeral");
    sequence.clear(); add(sequence, batch("ephemeral", action::erase));
    require(sequence.apply(*database) == status::ok &&
        missing->valid(unix_time_ms()), "missing erase dirtied WATCH");
    sequence.clear();
    add(sequence, batch("ephemeral", action::erase, "", false,
        {{"ephemeral", 1, "", action::erase}}));
    require(sequence.apply(*database) == status::ok,
        "missing-root page cleanup failed");
    require(missing->valid(unix_time_ms()),
        "cleanup pages dirtied missing WATCH");
    seed(database, "maintenance");
    auto token = watch(database, "maintenance");
    sequence.clear();
    add(sequence, batch("maintenance", action::write, encoded("old"),
        true, {}, true));
    require(sequence.apply(*database) == status::ok &&
        token->valid(unix_time_ms()), "pure maintenance dirtied WATCH");
    add(sequence, batch("maintenance", action::write, encoded("changed")));
    add(sequence, batch("maintenance", action::write, encoded("changed"),
        true, {}, true));
    require(sequence.apply(*database) == status::ok &&
        !token->valid(unix_time_ms()), "maintenance hid a logical write");
}
static void missing_maintenance(shared_db database) {
    require(database->replica_erase("missing-maintenance"),
        "missing maintenance reset");
    auto token = watch(database, "missing-maintenance");
    page_sequence sequence;
    auto migration = batch("missing-maintenance", action::write,
        encoded("arrived"), true, {}, true);
    add(sequence, migration);
    require(sequence.apply(*database) == status::ok &&
        !token->valid(unix_time_ms()),
        "maintenance created a key without invalidating missing WATCH");
    require(database->replica_erase("missing-maintenance"),
        "group maintenance reset");
    token = watch(database, "missing-maintenance");
    add(sequence, migration);
    require(sequence.apply(*database) == status::ok &&
        !token->valid(unix_time_ms()), "group maintenance lost creation");
    token = watch(database, "missing-maintenance");
    require(sequence.apply(*database) == status::ok &&
        token->valid(unix_time_ms()), "existing maintenance dirtied WATCH");
    require(database->replica_erase("missing-maintenance"),
        "transient maintenance reset");
    token = watch(database, "missing-maintenance");
    sequence.clear(); add(sequence, migration);
    add(sequence, batch("missing-maintenance", action::erase));
    require(sequence.apply(*database) == status::ok &&
        !token->valid(unix_time_ms()),
        "migration/delete lost creation history");
}
class measured_db : public mdb {
public:
    measured_db() : mdb(4) {}
    unsigned commits = 0, reads = 0;
    bool reject = false;
protected:
    lookup_status dblookup_page(const std::string& key, uint64_t id,
        std::string& value) override {
        ++reads; return mdb::dblookup_page(key, id, value);
    }
    batch_status dbapply_pages(const std::vector<batch_entry>& roots,
        const std::vector<page_change>& pages) override {
        ++commits;
        return reject ? status::error : mdb::dbapply_pages(roots, pages);
    }
};
static void failures() {
    bool failed_in_storage = false;
    bool succeeded = false;
    for (long allocation = 0; allocation < 500 && !succeeded; ++allocation) {
        auto concrete = std::make_shared<measured_db>();
        shared_db database = concrete;
        require(database->open(""), "fault DB open"); seed(database, "fault");
        auto token = watch(database, "fault");
        page_sequence sequence;
        add(sequence, batch("fault", action::write, encoded("new"), true,
            {{"fault", 2, std::string(16000, 'x'), action::write}}));
        add(sequence, batch("other", action::write, encoded("other")));
        concrete->commits = concrete->reads = 0;
        budget = allocation;
        const auto result = sequence.apply(*database);
        budget = -1;
        require(concrete->reads == 0 && concrete->commits <= 1,
            "sequence materialized old pages or committed a prefix");
        std::string out;
        if (result == status::ok) {
            succeeded = true;
            require(root(database, "fault") == encoded("new") &&
                !token->valid(unix_time_ms()), "successful fault sweep");
        } else {
            failed_in_storage = failed_in_storage || concrete->commits != 0;
            require(result == status::error &&
                root(database, "fault") == encoded("old") &&
                page(database, "fault", 99, out) && out == "old-99" &&
                token->valid(unix_time_ms()), "OOM exposed a partial commit");
        }
    }
    require(succeeded && failed_in_storage, "allocation sweep did not cover staging");
}
static void storage_failure() {
    auto concrete = std::make_shared<measured_db>();
    shared_db database = concrete;
    require(database->open(""), "rejected storage open");
    seed(database, "rejected");
    auto token = watch(database, "rejected");
    page_sequence sequence;
    add(sequence, batch("rejected", action::write, encoded("new"), true));
    add(sequence, batch("second", action::write, encoded("second")));
    concrete->reject = true; concrete->commits = 0;
    require(sequence.apply(*database) == status::error &&
        concrete->commits == 1 &&
        root(database, "rejected") == encoded("old") &&
        token->valid(unix_time_ms()), "rejected commit leaked a prefix");
    concrete->reject = false;
    require(sequence.apply(*database) == status::ok &&
        root(database, "rejected") == encoded("new") &&
        root(database, "second") == encoded("second"), "storage retry");
}
struct model_value {
    bool present = false;
    std::string metadata;
    std::map<uint64_t, std::string> pages;
};
static void random_model(shared_db database) {
    std::map<std::string, model_value> model;
    std::mt19937 random(0x504b5632);
    for (const auto& key : {"model-a", "model-b", "model-c"}) {
        require(database->replica_erase(key), "model reset");
        model.emplace(key, model_value());
    }
    for (unsigned epoch = 0; epoch < 100; ++epoch) {
        page_sequence sequence;
        for (unsigned step = 0; step < 20; ++step) {
            auto selected = model.begin(); std::advance(selected, random() % 3);
            const auto& key = selected->first;
            auto& expected = selected->second;
            const auto choice = random() % 4;
            if (choice == 0) {
                add(sequence, batch(key, action::erase));
                expected = model_value(); continue;
            }
            const bool reset = choice == 1;
            const bool write = reset || !expected.present || choice == 2;
            const auto operation = write ? action::write : action::keep;
            const auto value = encoded(std::to_string(random()));
            const uint64_t id = random() % 8 + 1;
            const bool erase = random() % 3 == 0;
            const auto bytes = erase ? std::string() : std::to_string(random());
            add(sequence, batch(key, operation, write ? value : "", reset,
                {{key, id, bytes, erase ? action::erase : action::write}}));
            if (reset) expected.pages.clear();
            if (write) { expected.present = true; expected.metadata = value; }
            if (erase) expected.pages.erase(id);
            else expected.pages[id] = bytes;
        }
        require(sequence.apply(*database) == status::ok, "model apply");
        for (const auto& item : model) {
            std::string bytes;
            const auto& expected = item.second;
            require(database->get(item.first, bytes) == expected.present &&
                (!expected.present || bytes == expected.metadata),
                "independent model root mismatch");
            for (uint64_t id = 1; id <= 8; ++id) {
                const auto found = expected.pages.find(id);
                require(page(database, item.first, id, bytes) ==
                    (found != expected.pages.end()) &&
                    (found == expected.pages.end() || bytes == found->second),
                    "independent model page mismatch");
            }
        }
    }
}
static void checkpoint(shared_db database, bool write) {
    if (write) {
        page_sequence sequence;
        add(sequence, batch("checkpoint-left", action::write, encoded("early"),
            true, {{"checkpoint-left", 2, "discarded", action::write}}));
        add(sequence, batch("checkpoint-right", action::write,
            encoded("complete")));
        add(sequence, batch("checkpoint-left", action::write,
            encoded("complete"), true,
            {{"checkpoint-left", 1, "complete-page", action::write}}));
        require(sequence.apply(*database) == status::ok, "checkpoint apply");
    }
    std::string value;
    require(root(database, "checkpoint-left") == encoded("complete") &&
        root(database, "checkpoint-right") == encoded("complete") &&
        page(database, "checkpoint-left", 1, value) &&
        value == "complete-page" &&
        !page(database, "checkpoint-left", 2, value), "checkpoint reopen");
}
static void limits() {
    std::vector<db::batch_entry> history;
    history.emplace_back("history", false, "");
    history.back().action = action::erase;
    history.back().force_watch_invalidation = true;
    require(!page_batch::capture(0, history, {}),
        "history flag silently flattened into standalone wire data");
    page_sequence denied;
    auto initial = batch("initial", action::write, "value");
    budget = 0;
    const bool appended = denied.append(initial);
    budget = -1;
    require(!appended && denied.size() == 0 && denied.bytes() == 0 &&
        denied.records() == 0, "failed append left partial accounting");
    page_sequence bytes;
    auto large = batch("bytes", action::write, std::string(16384, 'b'));
    while (bytes.append(large)) {}
    require(bytes.bytes() <= page_batch::max_bytes &&
        page_batch::max_bytes - bytes.bytes() < large->size().bytes &&
        bytes.records() < page_batch::max_records, "sequence byte budget");
    auto value = batch("limit", action::write, encoded("x"));
    page_sequence sequence;
    require(!sequence.append(nullptr) && sequence.size() == 0,
        "null append changed sequence");
    add(sequence, value);
    auto other = batch("limit", action::write, encoded("x"), false, {},
        false, 1);
    require(!sequence.append(other) && sequence.size() == 1,
        "mixed databases accepted");
    while (sequence.append(value)) {}
    require(sequence.records() <= page_batch::max_records &&
        sequence.bytes() <= page_batch::max_bytes && sequence.size() > 100,
        "sequence budget");
    sequence.clear();
    require(sequence.bytes() == 0 && sequence.records() == 0 &&
        sequence.size() == 0 && sequence.append(other) &&
        sequence.database() == 1, "sequence reset");
}
static void cross_store(shared_db database) {
    const std::string first = "cross-a";
    std::string second = "cross-b";
    const auto shard = [](const std::string& key) {
        return acl_hash_crc32(key.data(), key.size()) % 2;
    };
    while (shard(first) == shard(second)) second += 'x';
    require(database->set(first, encoded("first")) &&
        database->set(second, encoded("second")), "cross-store seed");
    auto a = watch(database, first), b = watch(database, second);
    page_sequence sequence;
    add(sequence, batch(first, action::write, encoded("changed-first")));
    add(sequence, batch(second, action::write, encoded("changed-second")));
    require(sequence.apply(*database) == status::unsupported &&
        root(database, first) == encoded("first") &&
        root(database, second) == encoded("second") &&
        a->valid(unix_time_ms()) && b->valid(unix_time_ms()),
        "cross-store rejection exposed a committed prefix");
}
int main(int argc, char** argv) {
    acl::acl_cpp_init();
    const std::string backend = argc > 1 ? argv[1] : "mdb";
    shared_db database;
    if (backend == "mdb") database = db::create_mdb(4);
    else if (backend == "tbb") database = db::create_mdb_tbb(4);
    else if (backend == "rdb") database = db::create_rdb();
    else if (backend == "wdb") database = db::create_wdb();
    const char* path = argc > 2 ? argv[2] : "";
    require(database && database->open(path), "sequence backend open");
    if (argc > 3 && std::string(argv[3]) == "--cross-store") {
        cross_store(database); return 0;
    }
    if (argc > 3 && std::string(argv[3]) == "--reopen") {
        checkpoint(database, false); return 0;
    }
    replacement(database); independent(database);
    prefix_validation(database); watch_history(database);
    missing_maintenance(database);
    random_model(database); failures(); storage_failure(); limits();
    checkpoint(database, true);
    std::cout << "Ordered page sequences passed: " << backend << '\n';
}
