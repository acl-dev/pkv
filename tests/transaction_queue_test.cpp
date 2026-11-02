#include "stdafx.h"
#include "common/transaction_queue.h"
#include "common/command_keys.h"
#include "coder/redis_coder.h"
#include "coder/redis_ocache.h"
#include <cstdlib>
#include <iostream>
#include <new>

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
static void require(bool ok, const char* message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}
static void snapshot() {
    redis_ocache cache;
    redis_coder source(cache);
    transaction_queue queue;
    require(queue.start() && !queue.start(), "MULTI state");
    auto& object = source.create_object();
    object.set_type(REDIS_OBJ_ARRAY);
    object.create_child().set_string("SET");
    object.create_child().set_string(std::string("a\0key", 5));
    object.create_child().set_string(std::string(50000, '\xff'));
    require(queue.append(object) == transaction_queue::result::queued,
        "queue snapshot");
    source.clear();
    source.create_object().set_string("parser reuse");
    auto commands = queue.take();
    require(!queue.active() && !queue.failed() && commands.size() == 1 &&
        commands[0] == std::vector<std::string>({"SET",
            std::string("a\0key", 5), std::string(50000, '\xff')}),
        "queued arguments borrowed the parser cache");
    transaction_queue limited(16, 1);
    auto& tiny = source.create_object(); tiny.set_type(REDIS_OBJ_ARRAY);
    tiny.create_child().set_string("PING");
    require(limited.start() && limited.append(tiny) ==
        transaction_queue::result::full && limited.failed(), "queue byte cap");
    limited.clear();
    require(!limited.active() && !limited.failed(), "DISCARD state");
}
static void allocation_failures() {
    bool succeeded = false;
    unsigned failed = 0;
    for (long allocation = 0; allocation < 32 && !succeeded; ++allocation) {
        redis_ocache cache;
        redis_coder source(cache);
        auto& object = source.create_object();
        object.set_type(REDIS_OBJ_ARRAY);
        object.create_child().set_string(std::string(5000, 'x'));
        transaction_queue queue;
        require(queue.start(), "queue failure start");
        budget = allocation;
        try { succeeded = queue.append(object) ==
            transaction_queue::result::queued; }
        catch (const std::bad_alloc&) {}
        budget = -1;
        if (!succeeded) {
            ++failed;
            require(queue.failed() && queue.take().empty(),
                "failed snapshot left a partial command");
        }
    }
    require(succeeded && failed >= 3, "queue allocation sweep");
}
static void reply_transfer() {
    redis_ocache cache;
    redis_coder source(cache), target(cache);
    auto& array = target.create_object(); array.set_type(REDIS_OBJ_ARRAY);
    for (unsigned i = 0; i < 3; ++i) array.create_child().set_number(i);
    source.create_object().set_status("first");
    source.create_object().set_number(4);
    source.create_object().set_null();
    source.move_to(array);
    require(source.get_objects().empty() && array.size() == 6,
        "reply transfer ownership");
    std::string wire;
    require(target.to_string(wire) && wire ==
        "*6\r\n:0\r\n:1\r\n:2\r\n+first\r\n:4\r\n$-1\r\n",
        "EXEC reply array bytes");
}
static void transfer_failures() {
    unsigned failures = 0;
    bool succeeded = false;
    for (long allocation = 0; allocation < 16 && !succeeded; ++allocation) {
        redis_ocache cache;
        redis_coder source(cache), target(cache);
        auto& array = target.create_object(); array.set_type(REDIS_OBJ_ARRAY);
        for (unsigned i = 0; i < 40; ++i)
            source.create_object().set_number(i);
        budget = allocation;
        try { source.move_to(array); succeeded = true; }
        catch (const std::bad_alloc&) { ++failures; }
        budget = -1;
        require(array.size() + source.get_objects().size() == 40,
            "reply transfer lost or duplicated ownership on OOM");
        std::string first, second;
        require(target.to_string(first) && source.to_string(second),
            "reply transfer damaged RESP ownership on OOM");
    }
    require(succeeded && failures >= 3, "reply transfer allocation sweep");
}
static void count_limit() {
    redis_ocache cache;
    redis_coder source(cache);
    auto& object = source.create_object(); object.set_type(REDIS_OBJ_ARRAY);
    object.create_child().set_string("PING");
    transaction_queue queue(1024, 1);
    require(queue.start() && queue.append(object) ==
        transaction_queue::result::queued && queue.append(object) ==
        transaction_queue::result::full && queue.failed() &&
        queue.take().size() == 1, "transaction command cap");
}
static void key_layouts() {
    struct sample {
        std::vector<std::string> arguments, keys;
        bool valid;
    };
    const std::vector<sample> samples = {
        {{"MSET", "a", "value", "b", "another"}, {"a", "b"}, true},
        {{"MSET", "a", "value", "incomplete"}, {}, false},
        {{"BITOP", "XOR", "out", "a", "b"}, {"out", "a", "b"}, true},
        {{"COPY", "a", "b", "DB", "1"}, {"a", "b"}, true},
        {{"SINTERCARD", "2", "a", "b", "LIMIT", "7"}, {"a", "b"}, true},
        {{"ZMPOP", "2", "a", "b", "MAX", "COUNT", "1"}, {"a", "b"}, true},
        {{"ZUNIONSTORE", "out", "2", "a", "b", "WEIGHTS", "1", "2"},
            {"out", "a", "b"}, true},
        {{"ZDIFF", "9223372036854775807", "a"}, {}, false},
        {{"LMPOP", "-1", "a", "LEFT"}, {}, false},
        {{"CONFIG", "GET", "*"}, {}, true},
        {{"ECHO", "looks-like-a-key"}, {}, true},
        {{"WATCH", "a", "b"}, {"a", "b"}, true},
        {{"HSCAN", "a", "0", "MATCH", "b"}, {"a"}, true},
    };
    for (const auto& sample : samples) {
        std::vector<const std::string*> keys;
        for (const auto& argument : sample.arguments) keys.push_back(&argument);
        require(command_keys(keys) == sample.valid, "command key count");
        std::vector<std::string> copied;
        for (const auto* key : keys) copied.push_back(*key);
        require(copied == sample.keys, "key routing treated an operand as key");
    }
}
int main() {
    acl::acl_cpp_init();
    snapshot(); allocation_failures(); reply_transfer();
    transfer_failures(); count_limit(); key_layouts();
    std::cout << "transaction queue snapshots/limits/replies passed\n";
}
