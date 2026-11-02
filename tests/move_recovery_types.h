#pragma once
#include "move_recovery_support.h"
#include "dao/paged_hash.h"
#include "dao/paged_list.h"
#include "dao/paged_set.h"
#include "dao/paged_zset.h"
#include "rpc/page_wire.h"
#include <zlib.h>

struct family_snapshot {
    std::string root;
    uint64_t pages = 0;
    uLong crc = 0;
};
inline family_snapshot snapshot(shared_db database, const std::string& key) {
    family_snapshot out;
    require(database->get(key, out.root) && out.root.size() <= 16384,
        "typed root not bounded");
    require(visit(database, key, [&](uint64_t id, const std::string& value) {
        char frame[16];
        page_wire::put64(frame, id); page_wire::put64(frame + 8, value.size());
        out.crc = ::crc32(out.crc, reinterpret_cast<const Bytef*>(frame), 16);
        out.crc = ::crc32(out.crc,
            reinterpret_cast<const Bytef*>(value.data()), value.size());
        ++out.pages; return true;
    }) == db::batch_status::ok, "typed family snapshot");
    return out;
}
inline void typed_recovery(shared_db& from, shared_db& to,
    const std::function<void(const std::string&)>& redo) {
    const std::string large(49152, 'v'), binary("member\0\xff", 8);
    {
        dao::paged_hash data;
        const std::map<std::string, std::string> fields{{binary, large},
            {"empty", ""}};
        require(data.hmset(from, "hash", fields) == 2, "typed hash seed");
        redo("hash");
        require(data.hgetall(to, "hash") && data.get_fields() == fields,
            "typed hash recovery");
        require(data.hset(to, "hash", "later", "x"), "typed hash mutation");
        std::string value;
        require(data.hget(to, "hash", binary, value) && value == large,
            "hash mutation damaged recovered blob");
    }
    {
        dao::paged_list data;
        const std::vector<std::string> values{"", binary, large};
        long long length = 0;
        require(data.push(from, "list", values, false, false, length) &&
            length == 3, "typed list seed");
        redo("list");
        std::vector<std::string> actual;
        require(data.range(to, "list", 0, -1, actual) && actual == values,
            "typed list recovery");
        require(data.push(to, "list", {"later"}, false, false, length) &&
            length == 4, "typed list mutation");
        auto expected = values; expected.push_back("later");
        require(data.range(to, "list", 0, -1, actual) && actual == expected,
            "list mutation damaged recovered blob");
    }
    {
        dao::paged_set data;
        const std::vector<std::string> members{binary, large, ""};
        long long changed = 0;
        require(data.change(from, "set", members, true, changed) &&
            changed == 3, "typed set seed");
        redo("set");
        std::set<std::string> actual;
        require(data.members(to, "set", actual) && actual ==
            std::set<std::string>(members.begin(), members.end()),
            "typed set recovery");
        require(data.change(to, "set", {"later"}, true, changed) &&
            changed == 1, "typed set mutation");
        auto expected = std::set<std::string>(members.begin(), members.end());
        expected.insert("later");
        require(data.members(to, "set", actual) && actual == expected,
            "set mutation damaged recovered blob");
    }
    {
        dao::paged_zset data;
        dao::zadd_result result;
        require(data.add(from, "zset", {{binary, -3.5}, {large, 2.0}},
            {}, result) && result.added == 2, "typed ZSet seed");
        redo("zset");
        std::vector<zset_member> actual;
        require(data.range(to, "zset", 0, -1, false, actual) &&
            actual.size() == 2 && actual[0].member == binary &&
            actual[0].score == -3.5 && actual[1].member == large &&
            actual[1].score == 2.0, "typed ZSet recovery");
        require(data.add(to, "zset", {{"later", 9}}, {}, result) &&
            result.added == 1, "typed ZSet mutation");
        require(data.range(to, "zset", 0, -1, false, actual) &&
            actual.size() == 3 && actual[0].member == binary &&
            actual[1].member == large && actual[2].member == "later" &&
            actual[2].score == 9, "ZSet mutation damaged recovered blob");
    }
    std::cout << "Hash/List/Set/ZSet bytes, large blobs and mutation passed\n";
}
