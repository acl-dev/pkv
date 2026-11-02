#include "stdafx.h"
#include "connection_input.h"
#include "client_memory.h"
#include <list>
#include <mutex>

namespace pkv {
struct connection_input::state {
    acl::socket_stream& socket;
    receive_callback received;
    std::function<void()> closed;
    const size_t limit;
    acl::fiber_mutex mutex;
    acl::fiber_cond ready;
    std::list<std::string> chunks;
    size_t offset = 0;
    std::atomic<size_t> bytes{0}, heap{0}, buffer{0};
    std::atomic<bool> ended{false};
    std::atomic<bool> started{false}, finished{false};
    bool notified = false;
    struct reader : acl::fiber {
        state& owner;
        explicit reader(state& input) : owner(input) {}
        void run() override {
            owner.started.store(true);
            owner.run();
            owner.finished.store(true);
        }
    };
    std::unique_ptr<reader> worker;
    state(acl::socket_stream& input, receive_callback on_receive,
        std::function<void()> on_close, size_t bound) : socket(input),
        received(std::move(on_receive)), closed(std::move(on_close)),
        limit(bound) {}
    std::unique_lock<acl::fiber_mutex> lock() {
        if (!mutex.lock()) std::abort();
        return std::unique_lock<acl::fiber_mutex>(mutex, std::adopt_lock);
    }
    static size_t memory(const std::string& chunk) {
        return string_heap_bytes(chunk) + sizeof(chunk) + 2 * sizeof(void*);
    }
    void finish() {
        bool notify;
        {
            auto guard = lock();
            ended = true; ready.notify();
            notify = !notified; notified = true;
        }
        if (notify) closed();
    }
    bool append(const char* data, size_t size) {
        auto guard = lock();
        if (ended || size > limit - bytes.load()) return false;
        chunks.emplace_back(data, size);
        const auto allocated = memory(chunks.back());
        if (allocated > limit - heap.load()) {
            chunks.pop_back(); return false;
        }
        bytes.fetch_add(size); heap.fetch_add(allocated);
        ready.notify(); return true;
    }
    void run() {
        try {
            std::vector<char> incoming(16384);
            buffer.store(incoming.capacity());
            while (!ended.load() && !acl::fiber::self_killed()) {
                const auto count = socket.read(incoming.data(),
                    incoming.size(), false);
                if (count <= 0) break;
                received(static_cast<size_t>(count));
                if (!append(incoming.data(), static_cast<size_t>(count))) break;
                acl::fiber::yield();
            }
        } catch (const std::bad_alloc&) {}
        buffer.store(0); finish();
    }
};
connection_input::connection_input(acl::socket_stream& socket,
    receive_callback received, std::function<void()> closed, size_t limit)
    : state_(new state(socket, std::move(received), std::move(closed),
        limit)) {}
connection_input::~connection_input() { stop(); }
void connection_input::start() {
    if (!acl_fiber_running())
        throw std::logic_error("input reader requires an owning fiber");
    if (state_->worker)
        throw std::logic_error("input reader already started");
    state_->worker.reset(new state::reader(*state_));
    state_->worker->start(64000);
}
void connection_input::stop() {
    state_->finish();
    if (state_->worker) {
        while (!state_->started.load()) acl::fiber::yield();
        if (!state_->finished.load()) state_->worker->kill(true);
        state_->worker.reset();
    }
}
int connection_input::read(char* data, size_t capacity) {
    if (!capacity) return 0;
    auto& input = *state_;
    auto guard = input.lock();
    while (input.chunks.empty() && !input.ended) {
        if (acl::fiber::self_killed()) return 0;
        input.ready.wait(input.mutex);
    }
    if (input.chunks.empty()) return 0;
    auto& first = input.chunks.front();
    const auto count = std::min(capacity, first.size() - input.offset);
    std::memcpy(data, first.data() + input.offset, count);
    input.offset += count; input.bytes.fetch_sub(count);
    if (input.offset == first.size()) {
        input.heap.fetch_sub(state::memory(first));
        input.chunks.pop_front(); input.offset = 0;
    }
    return static_cast<int>(count);
}
size_t connection_input::pending() const { return state_->bytes.load(); }
size_t connection_input::owned() const {
    return sizeof(*state_) + state_->heap.load() + state_->buffer.load();
}
bool connection_input::closed() const { return state_->ended.load(); }
} // namespace pkv
