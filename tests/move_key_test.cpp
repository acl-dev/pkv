#include "stdafx.h"
#include "dao/move_key.h"
#include "dao/native_import.h"
#include "db/database_pool.h"
#include "db/db_cursor.h"
#include <cstdlib>
#include <iostream>

using namespace pkv;
static void require(bool ok, const char* message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}
static shared_db memory(bool tbb) {
    auto out = tbb ? db::create_mdb_tbb(4) : db::create_mdb(4);
    require(out && out->open(""), "MOVE memory open"); return out;
}
static void payload(shared_db database, const std::string& key,
    const value_record& expected) {
    require(database->transact_pages({key}, [&](page_transaction& tx) {
        const auto& root = *tx.roots().find(key);
        require(root.found && root.value.size() <= 16384, "bounded MOVE root");
        record_header header;
        require(database->value_codec().stored_header(root.value, header) ==
            codec_status::ok && header.expires_at_ms ==
                expected.header.expires_at_ms, "MOVE expiry changed");
        switch (expected.header.kind) {
        case value_kind::string: {
            string_store store(tx, key); std::string value;
            return store.open() && store.read(value) &&
                value == expected.string_value;
        }
        case value_kind::list: {
            list_store store(tx, key); std::vector<std::string> values;
            return store.open() && store.range(0, -1, values) &&
                values == expected.list_value;
        }
        case value_kind::hash:
        case value_kind::set: {
            map_store store(tx, key, expected.header.kind == value_kind::hash
                ? map_kind::hash : map_kind::set);
            std::map<std::string, std::string> values;
            if (!store.open() || !store.visit([&](const std::string& name,
                const std::string& value) {
                values.emplace(name, value); return true;
            })) return false;
            if (expected.header.kind == value_kind::hash)
                return values == expected.hash_value;
            std::set<std::string> members;
            for (const auto& pair : values) members.insert(pair.first);
            return members == expected.set_value;
        }
        case value_kind::zset: {
            zset_store store(tx, key); std::map<std::string, double> values;
            return store.open() && store.visit([&](const std::string& name,
                double score) { values.emplace(name, score); return true; })
                && values == expected.zset_value;
        }
        }
        return false;
    }) == db::batch_status::ok, "MOVE legacy/native logical data");
}
static void legacy(bool tbb) {
    auto from = memory(tbb), to = memory(tbb);
    database_pool pool(from, 16, [&](size_t index) {
        return index == 7 ? to : shared_db();
    });
    for (auto kind : {value_kind::string, value_kind::list, value_kind::hash,
        value_kind::set, value_kind::zset}) {
        value_record record;
        record.header.kind = kind;
        record.header.expires_at_ms = 4102444800123LL;
        const std::string blob(70000, 'v');
        record.string_value = blob; record.list_value = {blob, ""};
        record.hash_value = {{"field", blob}, {"", "small"}};
        record.set_value = {blob, ""}; record.zset_value = {{blob, 1.5}};
        std::string encoded;
        require(from->value_codec().encode(record, encoded) ==
            codec_status::ok && encoded.size() > 16384, "legacy seed");
        const std::string key("legacy\0move", 11);
        require(from->set(key, encoded), "legacy root insert");
        dao::move_key move;
        bool moved = false;
        require(move.run(pool, nullptr, 0, 7, key, moved) && moved,
            move.error());
        std::string missing;
        require(!from->get(key, missing), "legacy source survived MOVE");
        payload(to, key, record);
        require(move.run(pool, nullptr, 7, 0, key, moved) && moved,
            "native reverse MOVE");
        payload(from, key, record);
        require(from->del(key), "legacy cycle cleanup");
    }
}
int main() {
    acl::acl_cpp_init(); legacy(false);
#ifdef HAS_TBB
    legacy(true);
#endif
    std::cout << "MOVE legacy inputs became bounded native families\n";
}
