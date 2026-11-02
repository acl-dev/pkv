#include "coder/redis_coder.h"
#include "coder/redis_ocache.h"
#include "common/client_registry.h"
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <thread>

using namespace pkv;
static void require(bool ok, const char* reason) {
    if (!ok) { std::cerr << reason << '\n'; std::exit(1); }
}
class input {
public:
    input() : cache_(64), parser_(cache_) {}
    void feed(const std::string& wire) {
        std::lock_guard<std::mutex> lock(mutex_);
        size_t remaining = wire.size();
        parser_.update(wire.data(), remaining);
        require(!remaining && !parser_.get_curr()->failed(), "input parse");
    }
    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        parser_.clear();
    }
    static client_runtime read(const void* context, long long) {
        const auto& self = *static_cast<const input*>(context);
        std::lock_guard<std::mutex> lock(self.mutex_);
        const auto memory = self.parser_.memory();
        client_runtime out;
        out.query_size = memory.partial.bytes;
        out.query_free = memory.partial.spare;
        out.arguments = memory.complete.bytes;
        out.owned_memory = memory.partial.owned + memory.complete.owned;
        return out;
    }
private:
    redis_ocache cache_;
    redis_coder parser_;
    mutable std::mutex mutex_;
};
int main() {
    client_registry registry;
    client_state metadata;
    std::unique_ptr<input> owner(new input);
    auto entry = registry.add(metadata.id(), 100, &metadata,
        client_inspector(owner.get(), &input::read));
    const std::string header("*2\r\n$4\r\nECHO\r\n$70000\r\n");
    owner->feed(header + std::string(111, 'x'));
    client_view view;
    require(entry->read_view(view, 0) && view.runtime.query_size >= 111 &&
        view.runtime.owned_memory >= view.runtime.query_size,
        "partial input memory");
    owner->feed(std::string(70000 - 111, 'x') + "\r\n");
    require(entry->read_view(view, 0) && !view.runtime.query_size &&
        view.runtime.arguments >= 70000 &&
        view.runtime.owned_memory >= view.runtime.arguments,
        "completed input memory");
    owner->clear();
    require(entry->read_view(view, 0) && !view.runtime.arguments &&
        !view.runtime.query_size, "cleared input memory");
    std::atomic<bool> done{false};
    std::atomic<size_t> reads{0};
    std::thread reader([&] {
        while (!done.load()) {
            client_view current;
            if (entry->read_view(current, 0)) {
                require(current.runtime.owned_memory >=
                    current.runtime.query_size + current.runtime.arguments,
                    "input memory visitor inconsistency");
                ++reads;
            }
        }
    });
    while (!reads.load()) std::this_thread::yield();
    const std::string small("*2\r\n$4\r\nECHO\r\n$4096\r\n");
    const std::string chunk(256, 'x');
    for (size_t i = 0; i < 1000; ++i) {
        owner->feed(small);
        for (size_t block = 0; block < 16; ++block) owner->feed(chunk);
        owner->feed("\r\n");
        owner->clear();
    }
    registry.retire(entry);
    owner.reset();
    for (size_t i = 0; i < 1000; ++i)
        require(!entry->read_view(view, 0), "retired input observer accessed");
    done = true;
    reader.join();
    std::cout << "Client input resources, fragmented parsing and lifetime "
        "passed\n";
}
