#include "stdafx.h"
#include "coder/record_codec.h"
#include "dao/paged_string.h"
#include "dao/paged_list.h"
#include "dao/paged_hash.h"
#include "dao/paged_set.h"
#include "dao/paged_zset.h"
#include "db/mdb/mdb.h"
#include "rpc/page_batch.h"
#include <cstdlib>
#include <iostream>

using namespace pkv;
static void require(bool ok, const char* message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}
static watch_token observe(shared_db database, const std::string& key) {
    std::vector<watch_token> tokens;
    require(database->watch({key}, tokens) == db::batch_status::ok &&
        tokens.size() == 1, "subscribe");
    return tokens.front();
}
static shared_db memory() {
    auto database = db::create_mdb(4);
    require(database->open(""), "open");
    return database;
}
static void migrations() {
    auto database = memory();
    for (const auto kind : {value_kind::string, value_kind::list,
        value_kind::hash, value_kind::set, value_kind::zset}) {
        value_record record;
        record.header.kind = kind;
        record.string_value = std::string(50000, 's');
        record.list_value = {std::string(50000, 'l'), "last"};
        record.hash_value = {{"field", std::string(50000, 'h')}};
        record.set_value = {std::string(50000, 'm')};
        record.zset_value = {{std::string(50000, 'z'), 3.5}};
        std::string bytes;
        require(database->value_codec().encode(record, bytes) ==
            codec_status::ok && database->set("key", bytes), "legacy seed");
        auto token = observe(database, "key");
        long long count = 0;
        bool ok = false;
        if (kind == value_kind::string) {
            dao::paged_string data;
            ok = data.length(database, "key", count);
        } else if (kind == value_kind::list) {
            dao::paged_list data;
            ok = data.length(database, "key", count);
        } else if (kind == value_kind::hash) {
            dao::paged_hash data;
            ok = data.hlen(database, "key", count);
        } else if (kind == value_kind::set) {
            dao::paged_set data;
            ok = data.size(database, "key", count);
        } else {
            dao::paged_zset data;
            ok = data.size(database, "key", count);
        }
        require(ok && count > 0 && token->valid(unix_time_ms()),
            "format migration invalidated WATCH");
        std::string native;
        require(database->get("key", native) && native != bytes,
            "read did not migrate legacy fixture");
        require(database->set("key", native) &&
            !token->valid(unix_time_ms()), "same-value SET must dirty WATCH");
    }
}
static void lifecycle() {
    auto database = memory();
    dao::paged_string data;
    require(data.set(database, "key", "value"), "native seed");
    auto old = observe(database, "key");
    auto other = observe(database, "key");
    long long length;
    require(data.append(database, "key", "", length) &&
        !old->valid(unix_time_ms()) && !other->valid(unix_time_ms()),
        "APPEND empty is a write");
    auto fresh = observe(database, "key");
    old.reset(); other.reset();
    require(data.set(database, "key", "changed") &&
        !fresh->valid(unix_time_ms()), "old subscriptions erased fresh epoch");
    auto missing = observe(database, "missing");
    require(database->update("missing", [](bool, std::string&) {
        return db::update_action::erase;
    }) && missing->valid(unix_time_ms()), "missing erase is a no-op");
    require(data.set(database, "missing", "now-present") &&
        !missing->valid(unix_time_ms()), "missing to present not tracked");
    std::vector<watch_token> tokens;
    require(database->set("zzbad", "invalid") && database->watch(
        {"key", "zzbad"}, tokens) != db::batch_status::ok && tokens.empty(),
        "invalid metadata must fail WATCH closed");
}
static void maintenance_replica() {
    auto database = memory();
    dao::paged_string data;
    require(data.set(database, "key", "same"), "replica seed");
    auto token = observe(database, "key");
    std::string bytes;
    require(database->get("key", bytes), "replica root");
    std::vector<db::batch_entry> roots;
    roots.emplace_back("key", true, bytes);
    roots.back().action = db::update_action::write;
    roots.back().clear_pages = true;
    roots.back().maintenance_only = true;
    auto batch = page_batch::capture(0, roots, {});
    require(batch && batch->roots().front().maintenance_only &&
        batch->apply(*database) == db::batch_status::ok &&
        token->valid(unix_time_ms()), "replica migration dirtied WATCH");
    roots.back().maintenance_only = false;
    batch = page_batch::capture(0, roots, {});
    require(batch && batch->apply(*database) == db::batch_status::ok &&
        !token->valid(unix_time_ms()), "replica write not tracked");
}
class failing_db : public mdb {
public:
    failing_db() : mdb(4) {}
    bool reject = false;
protected:
    batch_status dbapply_pages(const std::vector<batch_entry>& roots,
        const std::vector<page_change>& pages) override {
        return reject ? batch_status::error : mdb::dbapply_pages(roots, pages);
    }
};
static void failed_commit() {
    auto concrete = std::make_shared<failing_db>();
    shared_db database = concrete;
    require(database->open(""), "failure fixture open");
    dao::paged_string data;
    require(data.set(database, "key", "original"), "failure fixture seed");
    auto token = observe(database, "key");
    concrete->reject = true;
    require(!data.set(database, "key", "uncommitted") &&
        token->valid(unix_time_ms()), "failed commit invalidated WATCH");
    concrete->reject = false;
    std::string value;
    require(data.get(database, "key", value) && value == "original" &&
        data.set(database, "key", "committed") &&
        !token->valid(unix_time_ms()), "failure fixture recovery");
}
int main() {
    acl::acl_cpp_init();
    migrations(); lifecycle(); maintenance_replica(); failed_commit();
    std::cout << "WATCH migration, epochs and replica commits passed\n";
}
