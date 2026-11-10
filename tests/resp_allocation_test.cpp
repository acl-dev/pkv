#include "coder/redis_coder.h"
#include "coder/redis_ocache.h"
#include <atomic>
#include <climits>
#include <cstdlib>
#include <iostream>
#include <new>
#include <string>

static thread_local long budget = -1;
static std::atomic<long> live(0);
void* operator new(std::size_t size) {
    if (!budget) throw std::bad_alloc();
    if (budget > 0) --budget;
    if (void* p = std::malloc(size ? size : 1)) { ++live; return p; }
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* p) noexcept {
    if (p) { --live; std::free(p); }
}
void operator delete[](void* p) noexcept { ::operator delete(p); }
#if __cplusplus >= 201402L
void operator delete(void* p, std::size_t) noexcept { ::operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept { ::operator delete(p); }
#endif
static void require(bool ok, const char* text) {
    if (!ok) { std::cerr << text << '\n'; std::exit(1); }
}
static void exercise(unsigned mode, size_t capacity, const std::string& wire) {
    pkv::redis_ocache cache(capacity);
    pkv::redis_coder parser(cache), builder(cache);
    if (mode != 1) {
        size_t remaining = wire.size();
        parser.update(wire.data(), remaining);
        require(!remaining && parser.get_objects().size() == 2,
            "allocation control parse");
    }
    if (mode != 0) {
        auto& array = builder.create_object();
        array.set_type(pkv::REDIS_OBJ_ARRAY);
        for (unsigned i = 0; i < 24; ++i) {
            auto& pair = array.create_child();
            pair.set_type(pkv::REDIS_OBJ_ARRAY);
            pair.create_child().set_string(std::string(64, 'x'));
            pair.create_child().set_number(i);
        }
        if (mode == 2) parser.move_to(array);
    }
    std::string output;
    require(parser.to_string(output) && builder.to_string(output),
        "allocation control serialize");
}
static void inline_parse(size_t capacity, const std::string& wire) {
    pkv::redis_ocache cache(capacity);
    pkv::redis_coder parser(cache);
    for (size_t pos = 0; pos < wire.size(); ++pos) {
        size_t n = 1;
        parser.update(wire.data() + pos, n);
        require(!n && !parser.get_curr()->failed(), "inline allocation parse");
    }
    require(parser.get_objects().size() == 3, "inline allocation frames");
    std::string out;
    require(parser.to_string(out), "inline allocation serialize");
}

static void inline_allocations() {
    const std::string wire = "SET key " + std::string(64, 'x') +
        "\r\nSET 'key space' \"" + std::string(128, 'y') +
        "\"\r\n*1\r\n$4\r\nPING\r\n";
    unsigned total = 0;
    for (size_t capacity : {size_t(0), size_t(1), size_t(8), size_t(64)}) {
        const auto before = live.load();
        budget = LONG_MAX;
        inline_parse(capacity, wire);
        const auto count = LONG_MAX - budget;
        budget = -1;
        require(live == before, "inline allocation control leaked owner");
        for (long fault = 0; fault <= count; ++fault) {
            budget = fault;
            try { inline_parse(capacity, wire); }
            catch (const std::bad_alloc&) {}
            budget = -1;
            require(live == before, "inline allocation failure leaked owner");
            ++total;
        }
    }
    pkv::redis_ocache cache(128);
    pkv::redis_coder parser(cache);
    std::string pipeline;
    for (unsigned i = 0; i < 16; ++i) pipeline += "PING\r\n";
    auto parse = [&] {
        size_t n = pipeline.size(); parser.update(pipeline.data(), n);
        const bool ok = !n && parser.get_objects().size() == 16;
        parser.clear();
        return ok;
    };
    for (unsigned i = 0; i < 10; ++i) require(parse(), "inline warmup");
    budget = 0;
    for (unsigned i = 0; i < 100; ++i)
        require(parse(), "warm inline pipeline allocated");
    budget = -1;
    std::cout << "inline allocation positions: " << total <<
        "; warm inline pipeline: 0 allocations\n";
}

static void pending_reply_exercise(size_t capacity,
    const std::string& payload) {
    pkv::redis_ocache cache(capacity);
    pkv::redis_coder builder(cache);
    builder.create_object().set_status("before");
    {
        pkv::redis_coder::pending_reply outer(builder);
        outer.string() = payload;
        require(builder.get_objects().size() == 1,
            "read reply published before completion");
        {
            pkv::redis_coder::pending_reply nested(builder);
            nested.string() = "nested";
            require(outer.string() == payload, "nested read borrowed value");
            nested.publish();
        }
        builder.create_object().set_status("callback");
        outer.publish();
    }
    const auto expected = "+before\r\n$6\r\nnested\r\n+callback\r\n$" +
        std::to_string(payload.size()) + "\r\n" + payload + "\r\n";
    for (unsigned protocol : {2u, 3u}) {
        for (auto* object : builder.get_objects())
            object->response_protocol(protocol);
        std::string output;
        require(builder.to_string(output) && output == expected,
            "pending reply publication order or bytes changed");
    }
    {
        pkv::redis_coder::pending_reply abandoned(builder);
        abandoned.string() = payload;
    }
    std::string output;
    require(builder.to_string(output) && output == expected,
        "abandoned reply became visible");
    builder.clear();
    {
        pkv::redis_coder::pending_reply clean(builder);
        require(clean.string().empty(), "abandoned reply retained value");
        clean.object().set_null();
        clean.publish();
    }
    {
        pkv::redis_coder::pending_reply error(builder);
        error.string() = "partial";
        error.object().set_error("ERR test");
        error.publish();
    }
    output.clear();
    require(builder.to_string(output) && output == "$-1\r\n-ERR test\r\n",
        "missing or failed read leaked partial bytes");
}

static void pending_reply_allocations() {
    const std::string payload = std::string("a\0b\r\n", 5) +
        std::string(1019, 'x');
    unsigned total = 0;
    for (size_t capacity : {size_t(0), size_t(1), size_t(8), size_t(64)}) {
        const auto before = live.load();
        budget = LONG_MAX;
        pending_reply_exercise(capacity, payload);
        const auto count = LONG_MAX - budget;
        budget = -1;
        require(live == before, "pending reply control leaked owner");
        unsigned failures = 0, successes = 0;
        for (long fault = 0; fault <= count; ++fault) {
            budget = fault;
            try { pending_reply_exercise(capacity, payload); ++successes; }
            catch (const std::bad_alloc&) { ++failures; }
            budget = -1;
            require(live == before, "pending reply OOM leaked owner");
            ++total;
        }
        require(failures && successes, "pending reply OOM sweep incomplete");
        for (const auto& value : {std::string(), std::string(3, 'a'),
            std::string(64, 'b'), std::string(262144, 'c')})
            pending_reply_exercise(capacity, value);
    }
    pkv::redis_ocache cache(64);
    pkv::redis_coder builder(cache);
    auto read = [&] {
        for (unsigned i = 0; i < 16; ++i) {
            pkv::redis_coder::pending_reply reply(builder);
            reply.string().assign(payload.data(), payload.size());
            reply.publish();
        }
        builder.clear();
    };
    read(); read();
    budget = 0;
    for (unsigned i = 0; i < 100; ++i) read();
    budget = -1;
    std::cout << "pending reply allocation positions: " << total <<
        "; warm P16 1KB fill/publish: 0 allocations\n";
}

static void pending_reply_publication_failure() {
    for (size_t capacity : {size_t(0), size_t(1), size_t(64)}) {
        const auto before = live.load();
        {
            pkv::redis_ocache cache(capacity);
            pkv::redis_coder builder(cache);
            pkv::redis_coder::pending_reply reply(builder);
            reply.string().assign(1024, 'x');
            budget = 0;
            bool failed = false;
            try { reply.publish(); }
            catch (const std::bad_alloc&) { failed = true; }
            budget = -1;
            require(failed && builder.get_objects().empty() &&
                reply.object().get_buf() == std::string(1024, 'x'),
                "failed publish lost ownership or exposed reply");
            reply.publish();
            std::string output;
            require(builder.to_string(output) && output ==
                "$1024\r\n" + std::string(1024, 'x') + "\r\n",
                "reply publish retry failed");
        }
        require(live == before, "publication failure or retry leaked owner");
    }
}

static void bulk_allocation_state() {
    for (const auto& ending : {"\r\n", "\rX", "X\n"}) {
        const auto before = live.load();
        {
            pkv::redis_ocache cache(8);
            pkv::redis_coder parser(cache);
            size_t remaining = 4;
            parser.update("*1\r\n", remaining);
            require(!remaining, "bulk allocation setup failed");
            const auto wire = "$128\r\n" + std::string(128, 'x') + ending;
            remaining = wire.size();
            bool failed = false;
            budget = 0;
            try { parser.update(wire.data(), remaining); }
            catch (const std::bad_alloc&) { failed = true; }
            budget = -1;
            require(failed && remaining == 2 &&
                !parser.get_curr()->failed() &&
                parser.get_objects().empty(),
                "bulk allocation changed consumption or publication");
        }
        require(live == before, "bulk allocation failure leaked owner");
    }
}

static void streamed_bulk_growth() {
    std::string payload(128 * 1024, 'v');
    payload.back() = '\0';
    const auto initial = live.load();
    constexpr size_t chunk = 65535;
    for (bool inject_failure : {false, true}) {
        pkv::redis_ocache cache(1);
        pkv::redis_coder parser(cache);
        auto* value = parser.get_curr();
        size_t remaining = sizeof("$131072\r\n") - 1;
        budget = 0;
        value->update("$131072\r\n", remaining);
        budget = -1;
        require(!remaining && value->get_buf().empty(), "bulk header alone allocated storage");

        remaining = chunk;
        value->update(payload.data(), remaining);
        require(!remaining && !value->finish(), "streamed bulk first chunk");
        remaining = chunk;
        bool failed = false;
        budget = inject_failure ? 0 : 1;
        try { value->update(payload.data() + chunk, remaining); }
        catch (const std::bad_alloc&) { failed = true; }
        require(failed == inject_failure && !remaining && !value->finish(),
            "streamed bulk growth changed allocation failure state");
        if (failed) {
            budget = -1;
            require(value->get_buf().size() == chunk, "failed reserve changed bulk bytes");
            continue;
        }
        budget = 0;
        remaining = payload.size() - 2 * chunk;
        value->update(payload.data() + 2 * chunk, remaining);
        require(!remaining && !value->finish(), "streamed bulk published without terminator");
        remaining = 2;
        value->update("\r\n", remaining);
        budget = -1;
        require(!remaining && value->finish() && value->get_buf() == payload,
            "streamed bulk tail allocated or changed binary payload");
    }
    require(live == initial, "streamed bulk allocation failure leaked objects");
}

static void streamed_bulk_cache() {
    const std::string payload = std::string(128 * 1024 - 1, 'v') + '\0';
    const auto initial = live.load();
    {
        pkv::redis_ocache cache(8);
        auto parse = [&](pkv::redis_object* value) {
            const char header[] = "$131072\r\n";
            size_t n = sizeof(header) - 1;
            value->update(header, n);
            require(!n && value->get_buf().empty(), "bulk cache header published bytes");
            n = 32768;
            value->update(payload.data(), n);
            require(!n && !value->finish(), "bulk cache published a partial value");
            n = payload.size() - 32768;
            value->update(payload.data() + 32768, n);
            require(!n && !value->finish(), "bulk cache published without CRLF");
            n = 2;
            value->update("\r\n", n);
            require(!n && value->finish() && value->get_buf() == payload,
                "bulk cache changed binary payload");
        };

        pkv::redis_object* values[5];
        for (auto& value : values) {
            value = cache.get();
            parse(value);
        }
        for (auto* value : values) cache.put(value);
        budget = 0;
        for (size_t i = 0; i < 4; ++i) {
            values[i] = cache.get();
            parse(values[i]);
        }
        values[4] = cache.get();
        bool exhausted = false;
        try { parse(values[4]); }
        catch (const std::bad_alloc&) { exhausted = true; }
        budget = -1;
        require(exhausted, "bulk cache retained more than four buffers");
        for (size_t i = 1; i < 5; ++i) cache.put(values[i]);

        cache.activity();
        cache.trim_idle(8);
        const auto warm = live.load();
        budget = 0;
        cache.trim_idle(8);
        budget = -1;
        require(live == warm - 3 && values[0]->get_buf() == payload,
            "idle trim failed to reclaim returned bulk buffers or touched a live value");
        cache.put(values[0]);
    }
    require(live == initial, "bulk cache leaked backing storage");
}

static void raw_reply_recycling() {
    pkv::redis_ocache cache(1);
    auto* reply = cache.get();
    reply->set_raw("$4096\r\n" + std::string(4096, 'x') + "\r\n");
    const auto before = live.load();
    budget = 0;
    cache.put(reply);
    reply = cache.get();
    budget = -1;
    require(live == before - 1 && reply->get_buf().empty(),
        "recycled raw reply retained its encoded frame");
    cache.put(reply);
}

static void bulk_collection_protocols() {
    pkv::redis_ocache cache(1);
    auto* reply = cache.get();
    reply->set_bulk_collection({"", std::string("a\0b", 3)}, true);
    std::string resp2, resp3;
    const std::string body("2\r\n$0\r\n\r\n$3\r\na\0b\r\n", 18);
    require(reply->to_resp(resp2, 2) && resp2 == "*" + body &&
        reply->to_resp(resp3, 3) && resp3 == "~" + body,
        "bulk Set reply did not preserve protocol or bytes");
    cache.put(reply);
}

static void take_raw_frame_without_copy() {
    pkv::redis_ocache cache(4);
    pkv::redis_coder replies(cache);
    auto& reply = replies.create_object();
    reply.set_bulk_collection({std::string(512, 'x')}, true);
    reply.response_protocol(3);
    std::string output;
    const auto* payload = reply.get_buf().data();
    budget = 0;
    const bool ok = replies.take_frame(output);
    budget = -1;
    require(ok && output.data() == payload &&
        output == "~1\r\n$512\r\n" + std::string(512, 'x') + "\r\n",
        "raw Set frame transfer copied or changed reply bytes");
}

static void idle_cache_reclaims_only_returned_objects() {
    const auto initial = live.load();
    {
        pkv::redis_ocache cache(64);
        std::vector<pkv::redis_object*> objects;
        for (unsigned i = 0; i < 48; ++i) objects.push_back(cache.get());
        for (auto* object : objects) cache.put(object);
        objects.clear();
        auto* borrowed = cache.get();
        borrowed->set_string("borrowed");
        const auto warm = live.load();
        for (unsigned tick = 0; tick < 3; ++tick) {
            cache.activity();
            cache.trim_idle(8);
            require(live == warm, "active cache reclaimed objects");
        }
        budget = 0;
        cache.trim_idle(8);
        require(live == warm - 39 && borrowed->get_buf() == "borrowed",
            "idle cache retained excess objects or reclaimed a borrower");
        for (unsigned i = 0; i < 8; ++i) objects.push_back(cache.get());
        bool exhausted = false;
        try { cache.get(); } catch (const std::bad_alloc&) { exhausted = true; }
        budget = -1;
        require(exhausted, "idle cache did not retain exactly eight objects");
        for (auto* object : objects) cache.put(object);
        objects.clear();
        cache.put(borrowed);
        cache.activity(); cache.trim_idle(0);
        const auto returned = live.load();
        budget = 0; cache.trim_idle(0); budget = -1;
        require(live == returned - 9, "zero retention failed to reclaim objects");
    }
    require(live == initial, "idle cache leaked objects");
}

static void idle_cache_releases_large_backing_storage() {
    const auto initial = live.load();
    {
        pkv::redis_ocache cache(256);
        auto* wide = cache.get();
        wide->set_type(pkv::REDIS_OBJ_ARRAY);
        for (unsigned i = 0; i < 96; ++i) wide->create_child().set_number(i);
        auto* large = cache.get();
        const std::string payload(512, 'v');
        large->set_string(payload);
        auto* borrowed = cache.get();
        borrowed->set_string(payload);
        cache.put(wide);
        cache.put(large);
        const auto warm = live.load();
        cache.activity();
        cache.trim_idle(256);
        require(live == warm, "active cache released wide backing storage");
        budget = 0;
        cache.trim_idle(256);
        auto* reused_large = cache.get();
        auto* reused_wide = cache.get();
        budget = -1;
        require(live == warm - 3 && borrowed->get_buf() == payload &&
            reused_large == large && reused_large->get_buf().capacity() < payload.size() &&
            reused_wide == wide, "idle trim failed to release only cached backing storage");
        reused_wide->set_type(pkv::REDIS_OBJ_ARRAY);
        reused_wide->create_child().set_string(payload);
        require(reused_wide->size() == 1 && *reused_wide->argument(0) == payload,
            "reused cache object failed to rebuild its backing storage");
        cache.put(reused_large);
        cache.put(reused_wide);
        cache.put(borrowed);
    }
    require(live == initial, "idle backing storage test leaked objects");
}

static void cached_payload_budget() {
    const auto initial = live.load();
    {
        pkv::redis_ocache cache(8);
        const std::string payload(64 * 1024 - 128, 'x');
        auto* borrowed = cache.get();
        borrowed->set_string(payload);
        pkv::redis_object* replies[6];
        for (auto& reply : replies) {
            reply = cache.get();
            reply->set_string(payload);
        }
        const auto capacity = replies[0]->get_buf().capacity();
        const auto retained = (256 * 1024) / (capacity + 1);
        require(retained > 0 && retained < 6, "payload budget fixture");
        const auto before = live.load();
        budget = 0;
        for (auto* reply : replies) cache.put(reply);
        require(live == before - (6 - static_cast<long>(retained)),
            "returned payloads exceeded the cache budget");
        for (size_t i = 6; i > 0; --i) {
            auto* reply = cache.get();
            require(reply == replies[i - 1] && reply->get_buf().empty() &&
                (reply->get_buf().capacity() == capacity) == (i <= retained),
                "payload cache changed order or retained the wrong buffers");
        }
        budget = -1;
        for (auto* reply : replies) reply->set_string(payload);
        budget = 0;
        for (auto* reply : replies) cache.put(reply);
        cache.activity();
        cache.trim_idle(8);
        const auto warm = live.load();
        cache.trim_idle(8);
        budget = -1;
        require(live == warm - static_cast<long>(retained) &&
            borrowed->get_buf() == payload,
            "checkout or idle trim corrupted the payload budget");
        cache.put(borrowed);
    }
    require(live == initial, "payload budget leaked backing storage");
}

static void wide_array_growth() {
    const size_t arguments = 514;
    std::string wire;
    for (size_t i = 0; i < arguments; ++i) wire += "$1\r\nx\r\n";
    const auto initial = live.load();
    for (long allocations = 0; allocations <= 9; ++allocations) {
        pkv::redis_ocache cache(arguments + 2);
        std::vector<pkv::redis_object*> shells;
        for (size_t i = 0; i < arguments + 2; ++i) shells.push_back(cache.get());
        for (auto* shell : shells) cache.put(shell);
        pkv::redis_coder parser(cache);
        auto* array = parser.get_curr();
        size_t n = 6;
        budget = 0;
        array->update("*514\r\n", n);
        budget = -1;
        require(!n && !array->size(), "array header alone allocated child storage");

        bool failed = false;
        budget = allocations;
        try {
            n = wire.size() - 1;
            array->update(wire.data(), n);
            require(!n && array->size() == arguments - 1 && !array->finish(),
                "incomplete array published its last child");
            n = 1;
            array->update(wire.data() + wire.size() - 1, n);
        } catch (const std::bad_alloc&) { failed = true; }
        budget = -1;
        if (allocations == 9)
            require(!failed && !n && array->finish() && array->size() == arguments &&
                *array->argument(arguments - 1) == "x", "wide array exceeded its allocation budget");
    }
    require(live == initial, "wide array allocation failure leaked objects");
}

int main() {
    idle_cache_reclaims_only_returned_objects();
    idle_cache_releases_large_backing_storage();
    cached_payload_budget();
    take_raw_frame_without_copy();
    raw_reply_recycling();
    bulk_collection_protocols();
    bulk_allocation_state();
    streamed_bulk_growth();
    streamed_bulk_cache();
    wide_array_growth();
    inline_allocations();
    pending_reply_allocations();
    pending_reply_publication_failure();
    const std::string wire = "*3\r\n+OK\r\n*2\r\n$1\r\na\r\n:1\r\n$256\r\n" +
        std::string(256, 'v') + "\r\n+NEXT\r\n";
    unsigned total = 0;
    for (unsigned mode = 0; mode < 3; ++mode) {
        for (size_t capacity : {size_t(0), size_t(1), size_t(8), size_t(64)}) {
            const auto before = live.load();
            budget = LONG_MAX;
            exercise(mode, capacity, wire);
            const auto count = LONG_MAX - budget;
            budget = -1;
            require(live == before, "control leaked RESP objects");
            for (long fault = 0; fault <= count; ++fault) {
                budget = fault;
                try { exercise(mode, capacity, wire); }
                catch (const std::bad_alloc&) {}
                budget = -1;
                require(live == before, "failed allocation leaked RESP owner");
                ++total;
            }
        }
    }
    std::cout << "RESP allocation-budget positions: " << total << '\n';
}
