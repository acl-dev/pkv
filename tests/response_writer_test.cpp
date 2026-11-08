#include "common/response_writer.h"
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include "coder/redis_coder.h"
#include "coder/redis_ocache.h"

static thread_local long allocation_budget = -1;
void* operator new(std::size_t size) {
    if (!allocation_budget) throw std::bad_alloc();
    if (allocation_budget > 0) --allocation_budget;
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

static void require(bool ok, const char* reason) {
    if (!ok) { std::cerr << reason << '\n'; std::exit(1); }
}
static void buffer_ownership() {
    using cache = pkv::response_buffer_cache;
    cache buffers;
    const std::string payload(4096, 'a');
    {
        cache::frame outer(buffers);
        outer.bytes() = payload;
        const auto* address = outer.bytes().data();
        {
            cache::frame nested(buffers);
            require(nested.bytes().empty(), "nested borrowed live bytes");
            nested.bytes().assign(1024, 'b');
        }
        require(outer.bytes() == payload && outer.bytes().data() == address,
            "nested frame changed outer payload");
        require(buffers.owned() >= 1025, "nested cache not accounted");
        size_t written = 0;
        require(!pkv::response_write(outer.bytes().size(), written,
            [&](size_t offset, size_t count) {
                cache::frame inner(buffers);
                inner.bytes().assign(2048, 'c');
                require(outer.bytes().compare(offset, count,
                    payload, offset, count) == 0, "write bytes changed");
                return count / 2;
            }) && written == payload.size() / 2, "partial write lifetime");
    }
    require(buffers.owned() >= payload.size() + 1, "cache heap missing");
    std::shared_ptr<const std::string> queued;
    {
        cache::frame frame(buffers);
        require(frame.bytes().empty() && !buffers.owned(), "stale reply");
        frame.bytes() = payload;
        queued = std::make_shared<const std::string>(std::move(frame.bytes()));
    }
    {
        cache::frame frame(buffers);
        frame.bytes().assign(8192, 'd');
    }
    require(*queued == payload, "queue frame recycled");
    {
        cache::frame frame(buffers);
        frame.bytes().assign(cache::limit + 1, 'x');
    }
    require(!buffers.owned(), "oversized reply retained");
    try {
        cache::frame frame(buffers);
        frame.bytes() = payload;
        throw std::bad_alloc();
    } catch (const std::bad_alloc&) {}
    cache::frame clean(buffers);
    require(clean.bytes().empty(), "exception retained reply contents");
}

static void encoding_failure() {
    pkv::redis_ocache objects;
    pkv::redis_coder builder(objects);
    builder.create_object().set_string(std::string(4096, 'x'));
    pkv::response_buffer_cache buffers;
    bool failed = false;
    try {
        pkv::response_buffer_cache::frame frame(buffers);
        frame.bytes().assign(64, 'y');
        allocation_budget = 0;
        builder.to_string(frame.bytes());
    } catch (const std::bad_alloc&) { failed = true; }
    allocation_budget = -1;
    require(failed, "encoding allocation failure not exercised");
    pkv::response_buffer_cache::frame recovered(buffers);
    require(recovered.bytes().empty(), "failed encoding bytes retained");
    std::string expected;
    require(builder.to_string(expected) &&
        builder.to_string(recovered.bytes()) &&
        recovered.bytes() == expected, "encoding after OOM differs");
}

static void encoding_reuse() {
    pkv::redis_ocache objects;
    pkv::redis_coder builder(objects);
    pkv::response_buffer_cache buffers;
    for (unsigned protocol : {2u, 3u}) {
        builder.create_object().set_status("OK");
        builder.create_object().set_error("ERR test");
        builder.create_object().set_null();
        auto& array = builder.create_object();
        array.set_type(pkv::REDIS_OBJ_ARRAY);
        array.create_child().set_string(std::string("a\0b", 3));
        array.create_child().set_string("");
        array.create_child().set_number(-42);
        for (auto* object : builder.get_objects())
            object->response_protocol(protocol);
        std::string expected;
        require(builder.to_string(expected), "reference encoding failed");
        for (int repeat = 0; repeat < 3; ++repeat) {
            pkv::response_buffer_cache::frame frame(buffers);
            require(builder.to_string(frame.bytes()) &&
                frame.bytes() == expected, "reused RESP bytes differ");
        }
        builder.clear();
    }
}

static void flat_pipeline_frame() {
    pkv::redis_ocache objects;
    pkv::redis_coder builder(objects);
    const std::vector<std::string> values(16, std::string(128, 'x'));
    builder.create_object().set_bulk_collection(values);
    auto& set = builder.create_object().set_bulk_collection(values, true);
    set.response_protocol(3);
    builder.create_object().set_bulk_collection(values);
    std::string expected, frame;
    require(builder.to_string(expected), "flat pipeline reference");
    allocation_budget = 1;
    const bool encoded = builder.take_frame(frame);
    allocation_budget = -1;
    require(encoded && frame == expected, "flat pipeline reserve or wire bytes");
}

static void bulk_collection_lengths() {
    pkv::redis_ocache objects;
    pkv::redis_coder reference(objects), flat(objects);
    std::vector<std::string> values;
    auto& array = reference.create_object();
    array.set_type(pkv::REDIS_OBJ_ARRAY);
    for (size_t length : {0, 9, 10, 99, 100}) {
        values.emplace_back(length, '\0');
        array.create_child().set_string(values.back());
    }
    flat.create_object().set_bulk_collection(values);
    std::string expected, actual;
    require(reference.to_string(expected) && flat.take_frame(actual) &&
        actual == expected, "bulk collection length encoding");
}

int main() {
    buffer_ownership();
    encoding_reuse();
    flat_pipeline_frame();
    bulk_collection_lengths();
    encoding_failure();
    const size_t chunk = 16ULL * 1024 * 1024;
    for (size_t length : {size_t(0), size_t(1), chunk - 1, chunk, chunk + 1,
        size_t(INT_MAX), size_t(INT_MAX) + 1, size_t(INT_MAX) + 100}) {
        size_t expected = 0, written = 999, calls = 0;
        const bool ok = pkv::response_write(length, written,
            [&](size_t offset, size_t count) {
                require(offset == expected && count > 0 && count <= chunk &&
                    count <= INT_MAX && count <= length - offset,
                    "invalid chunk or offset");
                expected += count;
                ++calls;
                return count;
            });
        require(ok && written == length && expected == length &&
            calls == (length + chunk - 1) / chunk, "truncated response");
    }
    size_t written = 0, calls = 0;
    const bool partial = pkv::response_write(chunk * 3, written,
        [&](size_t, size_t count) {
            return ++calls == 2 ? count / 2 : count;
        });
    require(!partial && calls == 2 && written == chunk + chunk / 2,
        "partial write accounting");
    const bool closed = pkv::response_write(std::numeric_limits<size_t>::max(),
        written, [](size_t, size_t) { return size_t(0); });
    require(!closed && !written, "closed stream continued writing");
    const bool invalid = pkv::response_write(chunk, written,
        [](size_t, size_t count) { return count + 1; });
    require(!invalid && !written, "writer over-report accepted");
    std::cout << "Bounded response writes and signed-int boundary passed\n";
}
