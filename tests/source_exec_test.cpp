#include "stdafx.h"
#include "action/redis_handler.h"
#include "action/redis_service.h"
#include "action/redis_string.h"
#include "action/redis_hash.h"
#include "common/transaction_database.h"
#include "coder/redis_ocache.h"
#include "coder/record_codec.h"
#include "dao/paged_string.h"
#include "dao/paged_list.h"
#include "dao/paged_hash.h"
#include "dao/paged_set.h"
#include "dao/paged_zset.h"
#include "db/database_pool.h"
#include "db/mdb/mdb.h"
#include "db/rocksdb/rdb.h"
#include "db/wt/wdb.h"
#include <cstdlib>
#include <iostream>
#include <new>
#include <climits>
#include <sys/socket.h>
#include <unistd.h>

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
using commands = std::vector<std::vector<std::string>>;
static void require(bool ok, const char* message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}
class counted : public mdb {
public:
    counted() : mdb(4) {}
    size_t attempts = 0, commits = 0, raw = 0;
    bool reject = false, atomic = true;
protected:
    bool dbatomic_workspace() const override { return atomic; }
    bool dbset(const std::string& key, const std::string& value) override {
        ++raw; return mdb::dbset(key, value);
    }
    batch_status dbapply_pages(const std::vector<batch_entry>& roots,
        const std::vector<page_change>& pages) override {
        ++attempts;
        const auto result = reject ? batch_status::error :
            mdb::dbapply_pages(roots, pages);
        if (result == batch_status::ok) ++commits;
        return result;
    }
};
static std::shared_ptr<counted> memory() {
    auto out = std::make_shared<counted>();
    require(out->open(""), "open memory"); return out;
}
struct connection {
    redis_ocache cache{64};
    redis_coder parser{cache};
    redis_service service;
    acl::socket_stream writer, reader;
    std::shared_ptr<database_pool> pool;
    std::unique_ptr<redis_handler> handler;
    explicit connection(shared_db zero, shared_db one = nullptr) {
        int sockets[2];
        require(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0,
            "socketpair");
        require(writer.open(sockets[0]) && reader.open(sockets[1]), "streams");
        pool = std::make_shared<database_pool>(zero, 2,
            [one](size_t) { return one; });
        handler.reset(new redis_handler(service, pool, parser, writer));
    }
    void prepare(const commands& input) {
        parser.clear();
        std::string wire;
        for (const auto& args : input) {
            wire += '*' + std::to_string(args.size()) + "\r\n";
            for (const auto& arg : args)
                wire += '$' + std::to_string(arg.size()) + "\r\n" + arg +
                    "\r\n";
        }
        size_t size = wire.size(); parser.update(wire.data(), size);
        require(!size && !parser.get_curr()->failed(), "request parse");
    }
    std::string receive() {
        std::string out;
        char bytes[4096];
        for (;;) {
            const auto count = ::recv(reader.sock_handle(), bytes,
                sizeof(bytes), MSG_DONTWAIT);
            if (count <= 0) break;
            out.append(bytes, static_cast<size_t>(count));
        }
        return out;
    }
    std::string run(const commands& input) {
        prepare(input); require(handler->handle(), "handler failed");
        return receive();
    }
};
static void command_dao_lifetime() {
    connection client(memory());
    client.prepare({{"GET", "missing"}, {"HGET", "missing", "field"}});
    const auto& objects = client.parser.get_objects();
    require(objects.size() == 2, "DAO lifetime request parse");
    bool constructed = false;
    budget = 0;
    try {
        redis_string strings(*client.handler, *objects[0]);
        redis_hash hashes(*client.handler, *objects[1]);
        auto string = redis_string::bind("gEt");
        auto hash = redis_hash::bind("hGeT");
        auto copied = string;
        constructed = string && hash && copied;
    } catch (const std::bad_alloc&) {}
    budget = -1;
    require(constructed, "command DAO construction allocated");
    require(client.run({{"HSET", "single", "", ""},
        {"HSET", "single", "", "value"},
        {"HMSET", "single", "field", "a"},
        {"HSET", "single", "field", "b", "field", "c", "new", "d"},
        {"HGET", "single", "field"}, {"HLEN", "single"},
        {"HGET", "single", ""}}) ==
        ":1\r\n:0\r\n+OK\r\n:1\r\n$1\r\nc\r\n:3\r\n"
        "$5\r\nvalue\r\n", "single Hash write pipeline/counts");
    const auto replies = client.run({
        {"HSET", "hash", "field", "v"}, {"GET", "hash"},
        {"GET", "missing"}, {"SET", "str", "value"}, {"GET", "str"},
        {"HGET", "str", "field"}, {"HGET", "hash", "missing"},
        {"HGET", "hash", "field"}, {"HGETALL", "hash"},
        {"HGETALL", "missing"}, {"INCR", "str"},
        {"SET", "str", "7"}, {"INCR", "str"}});
    const std::string wrong_type =
        "-WRONGTYPE Operation against a key holding the wrong kind of value"
        "\r\n";
    require(replies == ":1\r\n" + wrong_type +
        "$-1\r\n+OK\r\n$5\r\nvalue\r\n" + wrong_type +
        "$-1\r\n$1\r\nv\r\n*2\r\n$5\r\nfield\r\n$1\r\nv\r\n*0\r\n"
        "-ERR value is not an integer or out of range\r\n+OK\r\n:8\r\n",
        "command DAO state leaked between pipelined requests");
    require(client.run({{"sEt", "alias", "abcdef"},
        {"sUbStR", "alias", "1", "3"},
        {"gEtRaNgE", "alias", "1", "3"},
        {"sEt", "number", "10"}, {"dEcRbY", "number", "3"},
        {"mSeTnX", "number", "99", "new", "x"},
        {"gEt", "number"}, {"hSeT", "mixed-hash", "f", "v"},
        {"hGeT", "mixed-hash", "f"}, {"mUlTi"},
        {"aPpEnD", "alias", "g"}, {"hLeN", "mixed-hash"},
        {"eXeC"}}) ==
        "+OK\r\n$3\r\nbcd\r\n$3\r\nbcd\r\n+OK\r\n:7\r\n:0\r\n"
        "$1\r\n7\r\n:1\r\n$1\r\nv\r\n+OK\r\n+QUEUED\r\n"
        "+QUEUED\r\n*2\r\n:7\r\n:1\r\n",
        "bound dispatch lost mixed-case, alias or EXEC semantics");
}
static std::string get(shared_db database, const std::string& key) {
    dao::paged_string strings; std::string out;
    require(strings.get(database, key, out), "missing String"); return out;
}
static void eligibility() {
    size_t database = 99;
    std::vector<const std::string*> legacy;
    const auto choose = [&](const commands& input, bool expected,
        size_t initial = 0) {
        database = 99;
        const bool found = transaction_database(input, initial, 16, false,
            database, legacy);
        require(found == expected, "database plan mismatch");
        return database;
    };
    require(choose({{"SELECT", "1"}, {"SET", "a", "v"},
        {"COPY", "a", "b", "DB", "1"}, {"SELECT", "0"}}, true) == 1,
        "noninitial database plan");
    require(choose({{"SELECT", "bad"}, {"SET", "a", "v"},
        {"SELECT", "999"}, {"GET", "a"}}, true) == 0,
        "invalid SELECT changed plan");
    choose({{"SET", "a", "v"}, {"SELECT", "1"}, {"GET", "b"}}, false);
    choose({{"COPY", "a", "b", "DB", "1"}}, false);
    choose({{"PING"}, {"SELECT", "1"}, {"ECHO", "x"}}, false);
    choose({{"CLUSTER", "SLOTS"}, {"SET", "a", "v"}}, false);
    require(choose({{"SET", "a", "v"},
        {"COPY", "a", "b", "DB", "-1"}}, true) == 0,
        "invalid COPY changed plan");
    require(choose({{"EXPIRE", "a", "1"}, {"PERSIST", "b"}}, true) == 0 &&
        legacy.empty(), "expiry still needs legacy fallback");
}
static void atomic_commands() {
    auto source = memory(); connection client(source);
    const auto reply = client.run({{"MULTI"}, {"SET", "a", "1"},
        {"SET", "b", "2"}, {"GET", "a"}, {"EXEC"}, {"PING"}});
    require(reply == "+OK\r\n+QUEUED\r\n+QUEUED\r\n+QUEUED\r\n"
        "*3\r\n+OK\r\n+OK\r\n$1\r\n1\r\n+PONG\r\n" &&
        source->commits == 1 && source->raw == 0 && get(source, "a") == "1" &&
        get(source, "b") == "2", "EXEC did not commit once/read own writes");
    require(client.run({{"MULTI"}, {"GET", "a"}, {"EXEC"}}) ==
        "+OK\r\n+QUEUED\r\n*1\r\n$1\r\n1\r\n" && source->commits == 1,
        "read-only EXEC committed");
    require(client.run({{"MULTI"}, {"SET", "c", "3"},
        {"LPUSH", "a", "wrong"}, {"SET", "d", "4"}, {"EXEC"}}).find(
        "-WRONGTYPE") != std::string::npos && source->commits == 2 &&
        get(source, "c") == "3" && get(source, "d") == "4",
        "runtime error stopped staged successes");
    require(client.run({{"SET", "outside", "live"}}) == "+OK\r\n" &&
        get(source, "outside") == "live" && source->commits == 3,
        "finished workspace escaped EXEC");
}
static void selection() {
    auto zero = memory(), one = memory(); connection client(zero, one);
    const auto reply = client.run({{"MULTI"}, {"SELECT", "1"},
        {"SET", "a", "v"}, {"COPY", "a", "b", "DB", "1"},
        {"GET", "b"}, {"SELECT", "0"}, {"EXEC"}, {"GET", "a"}});
    require(reply == "+OK\r\n+QUEUED\r\n+QUEUED\r\n+QUEUED\r\n"
        "+QUEUED\r\n+QUEUED\r\n*5\r\n+OK\r\n+OK\r\n:1\r\n"
        "$1\r\nv\r\n+OK\r\n$-1\r\n" && zero->commits == 0 &&
        one->commits == 1 && get(one, "b") == "v",
        "SELECT/COPY escaped target workspace");
    require(client.run({{"MULTI"}, {"SET", "cross-a", "0"},
        {"SELECT", "1"}, {"SET", "cross-b", "1"}, {"EXEC"}}).find(
        "*3\r\n+OK\r\n+OK\r\n+OK\r\n") != std::string::npos &&
        zero->commits == 1 && one->commits == 2,
        "cross-DB legacy behavior changed");
}
static void rejected_commit() {
    auto source = memory(); connection client(source);
    std::vector<watch_token> tokens;
    require(source->watch({"a", "b"}, tokens) == db::batch_status::ok, "WATCH");
    source->reject = true;
    require(client.run({{"MULTI"}, {"SET", "a", "1"},
        {"SET", "b", "2"}, {"EXEC"}, {"PING"}}) ==
        "+OK\r\n+QUEUED\r\n+QUEUED\r\n-ERR EXEC storage commit failed\r\n"
        "+PONG\r\n" && source->attempts == 1 && source->commits == 0,
        "failed commit acknowledged commands");
    std::string value;
    require(!source->get("a", value) && !source->get("b", value) &&
        tokens[0]->valid(unix_time_ms()) && tokens[1]->valid(unix_time_ms()),
        "failed commit changed source/WATCH");
    source->reject = false;
    require(client.run({{"SET", "after", "ok"}}) == "+OK\r\n" &&
        get(source, "after") == "ok" && source->commits == 1,
        "rejected workspace remained attached");
}
static void compatibility() {
    auto source = memory(); connection client(source);
    source->atomic = false;
    require(client.run({{"MULTI"}, {"SET", "a", "1"},
        {"SET", "b", "2"}, {"EXEC"}}).find("*2\r\n+OK\r\n+OK\r\n") !=
        std::string::npos && source->commits == 2, "commit-domain fallback");
    source->atomic = true;
    value_record record; record.header.kind = value_kind::list;
    record.list_value = {std::string(70000, 'x')}; std::string bytes;
    require(source->value_codec().encode(record, bytes) ==
        codec_status::ok &&
        source->set("legacy", bytes), "legacy seed");
    const auto raw = source->raw, commits = source->commits;
    require(client.run({{"MULTI"}, {"PEXPIRE", "legacy", "600000"},
        {"SET", "new", "ok"}, {"EXEC"}}).find("*2\r\n:1\r\n+OK\r\n") !=
        std::string::npos && source->raw == raw &&
        source->commits == commits + 1,
        "legacy expiry was rejected by workspace bounds");
    require(source->set("legacy", bytes), "legacy rollback seed");
    std::vector<watch_token> tokens;
    require(source->watch({"legacy"}, tokens) == db::batch_status::ok,
        "legacy rollback WATCH");
    source->reject = true;
    const auto reply = client.run({{"MULTI"},
        {"PEXPIRE", "legacy", "600000"}, {"SET", "uncommitted", "x"},
        {"EXEC"}});
    source->reject = false;
    std::string actual;
    require(reply.find("-ERR EXEC storage commit failed") !=
        std::string::npos && source->get("legacy", actual) &&
        actual == bytes && source->lookup("uncommitted", actual) ==
            db::lookup_status::missing && tokens[0]->valid(unix_time_ms()),
        "failed EXEC exposed legacy migration");
}
static void legacy_expiry_boundary() {
    auto source = memory(); connection client(source);
    value_record record; record.header.kind = value_kind::list;
    record.list_value = {""}; std::string bytes;
    require(source->value_codec().encode(record, bytes) == codec_status::ok,
        "boundary overhead");
    record.list_value[0].assign(16380 - bytes.size(), 'x');
    require(source->value_codec().encode(record, bytes) == codec_status::ok &&
        bytes.size() == 16380 && source->set("tight", bytes), "boundary seed");
    const auto reply = client.run({{"MULTI"},
        {"PEXPIRE", "tight", "1000000000000000000"},
        {"SET", "after-tight", "ok"}, {"EXEC"}});
    require(reply.find("*2\r\n:1\r\n+OK\r\n") != std::string::npos &&
        get(source, "after-tight") == "ok", "legacy TTL growth rejected");
}
static void legacy_transfers() {
    auto source = memory(); connection client(source);
    value_record record; record.header.kind = value_kind::list;
    record.list_value = {std::string(70000, 'x')}; std::string bytes;
    require(source->value_codec().encode(record, bytes) == codec_status::ok &&
        source->set("old-list", bytes), "legacy transfer seed");
    const auto raw = source->raw, commits = source->commits;
    const auto input = commands{{"MULTI"}, {"COPY", "old-list", "copied"},
        {"RENAME", "old-list", "moved"}, {"LLEN", "copied"},
        {"LLEN", "moved"}, {"SET", "after-copy", "ok"}, {"EXEC"}};
    require(client.run(input).find("*5\r\n:1\r\n+OK\r\n:1\r\n:1\r\n"
        "+OK\r\n") != std::string::npos && source->raw == raw &&
        source->commits == commits + 1, "legacy transfer EXEC prefix");
    dao::paged_list lists; std::vector<std::string> out;
    auto database = client.pool->get(0);
    require(lists.range(database, "moved", 0, -1, out) &&
        out == record.list_value, "EXEC moved legacy payload");
    require(source->set("old-list", bytes), "legacy transfer rollback seed");
    std::vector<watch_token> tokens;
    require(source->watch({"old-list", "copied", "moved"}, tokens) ==
        db::batch_status::ok, "transfer rollback WATCH");
    source->reject = true;
    const auto reply = client.run(input);
    source->reject = false;
    std::string current;
    require(reply.find("-ERR EXEC storage commit failed") !=
        std::string::npos && source->get("old-list", current) &&
        current == bytes, "failed EXEC exposed legacy transfer");
    for (const auto& token : tokens)
        require(token->valid(unix_time_ms()), "failed transfer dirtied WATCH");
}
static void allocation_failures() {
    size_t completed = 0, failed = 0;
    for (long fault = 0; fault < 260; ++fault) {
        auto source = memory(); connection client(source);
        client.prepare({{"MULTI"}, {"SET", "a", "1"},
            {"SET", "b", "2"}, {"GET", "a"}, {"EXEC"}});
        bool ok = false; budget = fault;
        try { ok = client.handler->handle(); } catch (const std::bad_alloc&) {}
        budget = -1;
        if (ok) ++completed; else ++failed;
        std::string a, b;
        const bool first = source->get("a", a), second = source->get("b", b);
        require(first == second && source->commits <= 1 && source->raw == 0,
            "allocation failure exposed a source EXEC prefix");
    }
    require(completed && failed > 20, "allocation sweep missed source paths");
}
static long transfer_allocation_count() {
    auto source = memory(); connection client(source);
    value_record record; record.header.kind = value_kind::list;
    record.list_value = {std::string(70000, 'x')}; std::string bytes;
    require(source->value_codec().encode(record, bytes) == codec_status::ok &&
        source->set("legacy-alloc", bytes), "allocation count seed");
    client.prepare({{"MULTI"}, {"COPY", "legacy-alloc", "copy"},
        {"RENAME", "legacy-alloc", "moved"}, {"EXEC"}});
    budget = LONG_MAX;
    const bool ok = client.handler->handle();
    const long count = LONG_MAX - budget;
    budget = -1;
    require(ok && source->commits == 1 && count > 0 && count < 3000,
        "source transfer allocation count");
    return count;
}
static void transfer_allocation_failures() {
    const long count = transfer_allocation_count();
    unsigned completed = 0, rejected = 0;
    for (long fault = 0; fault <= count; ++fault) {
        auto source = memory(); connection client(source);
        value_record record; record.header.kind = value_kind::list;
        record.list_value = {std::string(70000, 'x')}; std::string bytes;
        require(source->value_codec().encode(record, bytes) ==
            codec_status::ok && source->set("legacy-alloc", bytes),
            "source transfer allocation seed");
        std::vector<watch_token> tokens;
        require(source->watch({"legacy-alloc"}, tokens) ==
            db::batch_status::ok, "allocation source WATCH");
        client.prepare({{"MULTI"}, {"COPY", "legacy-alloc", "copy"},
            {"RENAME", "legacy-alloc", "moved"}, {"EXEC"}});
        budget = fault;
        try { client.handler->handle(); } catch (const std::bad_alloc&) {}
        budget = -1;
        require(source->commits <= 1 && source->raw == 1,
            "source transfer bypassed workspace");
        std::string current;
        if (source->commits) {
            ++completed;
            require(source->lookup("legacy-alloc", current) ==
                db::lookup_status::missing &&
                source->lookup("copy", current) == db::lookup_status::found &&
                source->lookup("moved", current) == db::lookup_status::found &&
                !tokens[0]->valid(unix_time_ms()), "committed transfer prefix");
        } else {
            ++rejected;
            require(source->get("legacy-alloc", current) && current == bytes &&
                source->lookup("copy", current) == db::lookup_status::missing &&
                source->lookup("moved", current) ==
                    db::lookup_status::missing &&
                tokens[0]->valid(unix_time_ms()), "failed transfer prefix");
        }
    }
    require(completed && rejected, "source transfer sweep coverage");
    std::cout << "Source transfer allocation positions: " << count + 1 << '\n';
}
static commands metadata_commands() {
    return {{"MULTI"}, {"SET", "meta-before", "1"}, {"SELECT", "1"},
        {"COMMAND", "INFO", "GET"}, {"SELECT", "0"},
        {"SET", "meta-after", "2"}, {"EXEC"}};
}
static long metadata_allocation_count() {
    auto source = memory(), other = memory(); connection client(source, other);
    client.prepare(metadata_commands());
    budget = LONG_MAX;
    const bool ok = client.handler->handle();
    const long count = LONG_MAX - budget;
    budget = -1;
    require(ok && source->commits == 1 && !other->commits && count > 0 &&
        count < 3000, "metadata transaction commit/admission");
    require(get(source, "meta-before") == "1" &&
        get(source, "meta-after") == "2", "metadata transaction values");
    return count;
}
static void metadata_allocation_failures() {
    const long count = metadata_allocation_count();
    unsigned completed = 0, rejected = 0;
    for (long fault = 0; fault <= count; ++fault) {
        auto source = memory(), other = memory();
        connection client(source, other);
        client.prepare(metadata_commands());
        budget = fault;
        try { client.handler->handle(); } catch (const std::bad_alloc&) {}
        budget = -1;
        std::string bytes;
        const bool first = source->get("meta-before", bytes);
        const bool second = source->get("meta-after", bytes);
        require(first == second && source->commits <= 1 &&
            (source->commits == 1) == first && !other->commits &&
            !source->raw && !other->raw, "metadata failure exposed prefix");
        if (first) ++completed; else ++rejected;
    }
    require(completed && rejected, "metadata allocation coverage");
    std::cout << "Metadata EXEC allocation positions: " << count + 1 << '\n';
}
static commands durable_commands() {
    return {{"MULTI"}, {"SET", "first", "one"},
        {"SET", "second", "two"}, {"SET", "large", std::string(70000, 'x')},
        {"LPUSH", "list", "member"}, {"HSET", "hash", "field", "value"},
        {"SADD", "set", "member"}, {"ZADD", "zset", "1", "member"},
        {"EXEC"}};
}
template<class Engine> class crash_db : public Engine {
public:
    explicit crash_db(bool before) : before_(before) {}
protected:
    typename db::batch_status dbapply_pages(
        const std::vector<db::batch_entry>& roots,
        const std::vector<page_change>& pages) override {
        if (before_) ::_exit(71);
        const auto result = Engine::dbapply_pages(roots, pages);
        if (result == db::batch_status::ok) ::_exit(72);
        return result;
    }
private:
    bool before_;
};
static void verify_crash(shared_db source, bool committed) {
    for (const auto* key : {"first", "second", "large", "list", "hash", "set",
        "zset"}) {
        std::string value;
        const auto expected = committed ? db::lookup_status::found :
            db::lookup_status::missing;
        require(source->lookup(key, value) == expected,
            "recovered EXEC prefix");
    }
    if (committed) {
        require(get(source, "first") == "one" &&
            get(source, "second") == "two" &&
            get(source, "large") == std::string(70000, 'x'),
            "recovered String");
        dao::paged_list list; dao::paged_hash hash;
        dao::paged_set set; dao::paged_zset zset;
        long long length; std::string value;
        require(list.length(source, "list", length) && length == 1 &&
            hash.hget(source, "hash", "field", value) && value == "value" &&
            set.size(source, "set", length) && length == 1,
            "recovered List/Hash/Set");
        std::vector<zset_score> scores;
        require(zset.scores(source, "zset", {"member"}, scores) &&
            scores.size() == 1 && scores[0].found && scores[0].score == 1,
            "recovered ZSet pages");
    }
}
int main(int argc, char** argv) {
    acl::acl_cpp_init();
    if (argc > 3) {
        const std::string backend = argv[1], phase = argv[3];
        const bool hook = phase == "before" || phase == "after";
        const bool writer = hook || phase == "ack";
        shared_db source;
#ifdef HAS_ROCKSDB
        if (backend == "rdb") source = hook ?
            shared_db(new crash_db<rdb>(phase == "before")) : db::create_rdb();
#endif
#ifdef HAS_WT
        if (backend == "wdb") source = hook ?
            shared_db(new crash_db<wdb>(phase == "before")) : db::create_wdb();
#endif
        require(source && source->open(argv[2]), "crash source open");
        if (writer) {
            connection client(source);
            const auto reply = client.run(durable_commands());
            if (phase == "ack") {
                require(reply.find("*7\r\n+OK\r\n+OK\r\n+OK\r\n:1\r\n"
                    ":1\r\n:1\r\n:1\r\n") != std::string::npos,
                    "EXEC not acknowledged");
                ::_exit(73);
            }
            require(false, "crash hook not reached");
        } else verify_crash(source, phase != "verify-before");
        return 0;
    }
    command_dao_lifetime();
    eligibility(); atomic_commands(); selection(); rejected_commit();
    compatibility(); legacy_expiry_boundary(); legacy_transfers();
    allocation_failures(); transfer_allocation_failures();
    metadata_allocation_failures();
    std::cout << "Source EXEC atomic commit tests passed\n";
}
