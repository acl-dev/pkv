#include "stdafx.h"
#include "connection_input.h"
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
    struct chunk {
        std::unique_ptr<char[]> data;
        size_t size;
    };
    // Keep full 16 KiB socket reads in a 16 KiB allocation. These are wire
    // bytes, so a string's trailing terminator would only add allocation cost.
    std::list<chunk> chunks;
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
    static size_t memory(const chunk& data) {
        return data.size + sizeof(chunk) + 2 * sizeof(void*);
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
    static constexpr size_t read_size = 16384;
    bool append(std::unique_ptr<char[]>& incoming, size_t size) {
        auto guard = lock();
        const auto available = limit - heap.load();
        const auto overhead = sizeof(chunk) + 2 * sizeof(void*);
        if (ended || overhead > available || size > available - overhead)
            return false;
        std::unique_ptr<char[]> data;
        if (size == read_size) {
            data = std::move(incoming);
            buffer.store(0);
        } else {
            data.reset(new char[size]);
            std::memcpy(data.get(), incoming.get(), size);
        }
        chunks.push_back({std::move(data), size});
        const auto allocated = memory(chunks.back());
        bytes.fetch_add(size); heap.fetch_add(allocated);
        ready.notify(); return true;
    }
    void run() {
        try {
            std::unique_ptr<char[]> incoming;
            while (!ended.load() && !acl::fiber::self_killed()) {
                if (!incoming) {
                    incoming.reset(new char[read_size]);
                    buffer.store(read_size);
                }
                const auto count = socket.read(incoming.get(), read_size, false);
                if (count <= 0) break;
                received(static_cast<size_t>(count));
                if (!append(incoming, static_cast<size_t>(count))) break;
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
    const auto count = std::min(capacity, first.size - input.offset);
    std::memcpy(data, first.data.get() + input.offset, count);
    input.offset += count; input.bytes.fetch_sub(count);
    if (input.offset == first.size) {
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
