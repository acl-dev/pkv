#include "stdafx.h"
#include "connection_output.h"
#include "client_memory.h"
#include "response_writer.h"
#include <list>
#include <mutex>

namespace pkv {
struct connection_output::state {
    struct item {
        frame data;
        ticket reply;
        bool self;
        std::list<item> before;
        item(frame value, ticket id, bool origin) : data(std::move(value)),
            reply(id), self(origin) {}
    };
    send_callback send;
    sent_callback sent;
    std::function<void()> closed;
    size_t limit;
    acl::fiber_mutex mutex;
    acl::fiber_cond ready, delivered;
    std::list<item> queue;
    std::list<item>::iterator pending_reply;
    ticket next = 0, pending_id = 0, completed = 0;
    std::atomic<size_t> bytes{0}, heap{0}, count{0};
    std::atomic<bool> ended{false};
    bool finished = false;
    struct writer : acl::fiber {
        state& owner;
        explicit writer(state& output) : owner(output) {}
        void run() override {
            owner.run();
            auto guard = owner.lock();
            owner.finished = true; owner.delivered.notify();
        }
    };
    std::unique_ptr<writer> worker;
    state(send_callback sender, sent_callback on_sent,
        std::function<void()> on_close, size_t bound) :
        send(std::move(sender)), sent(std::move(on_sent)),
        closed(std::move(on_close)), limit(bound) {}
    std::unique_lock<acl::fiber_mutex> lock() {
        if (!mutex.lock()) std::abort();
        return std::unique_lock<acl::fiber_mutex>(mutex, std::adopt_lock);
    }
    static size_t node_memory() { return sizeof(item) + 2 * sizeof(void*); }
    static size_t payload_memory(const frame& data) {
        // Charge shared payloads to each recipient. This is conservative and
        // keeps a small local quota from pinning arbitrarily large frames.
        return data ? sizeof(std::string) + 4 * sizeof(void*) +
            string_heap_bytes(*data) : 0;
    }
    bool fits(size_t extra_bytes, size_t extra_heap) const {
        return extra_bytes <= limit - bytes.load() &&
            extra_heap <= limit - heap.load();
    }
    void add(const frame& data, bool node) {
        bytes.fetch_add(data ? data->size() : 0);
        heap.fetch_add(payload_memory(data) + (node ? node_memory() : 0));
        if (node) count.fetch_add(1);
    }
    void remove(const item& value) {
        bytes.fetch_sub(value.data ? value.data->size() : 0);
        heap.fetch_sub(node_memory() + payload_memory(value.data));
        count.fetch_sub(1);
    }
    void discard(std::list<item>& items) {
        for (auto& value : items) {
            discard(value.before); remove(value);
        }
        items.clear();
    }
    void finish() {
        bool notify = false;
        {
            auto guard = lock();
            if (!ended.exchange(true)) {
                pending_id = 0; discard(queue); notify = true;
            }
            ready.notify(); delivered.notify();
        }
        if (notify) closed();
    }
    bool take(std::list<item>& work) {
        auto guard = lock();
        while (!ended && (queue.empty() || !queue.front().data)) {
            if (acl::fiber::self_killed()) return false;
            ready.wait(mutex);
        }
        if (ended) return false;
        work.splice(work.end(), queue.front().before);
        work.splice(work.end(), queue, queue.begin());
        return true;
    }
    void run() {
        std::list<item> work;
        try {
            while (take(work)) {
                while (!work.empty() && !ended.load()) {
                    const auto& value = work.front();
                    size_t written = 0;
                    bool ok = false;
                    try {
                        ok = response_write(value.data->size(), written,
                            [&](size_t offset, size_t length) {
                                return send(value.data->data() + offset,
                                    length);
                            });
                    } catch (const std::bad_alloc&) {}
                    sent(written, value.reply != 0);
                    {
                        auto guard = lock();
                        if (ok && value.reply) completed = value.reply;
                        remove(value); work.pop_front(); delivered.notify();
                    }
                    if (!ok) { finish(); break; }
                    acl::fiber::yield();
                }
            }
        } catch (const std::bad_alloc&) {}
        {
            auto guard = lock();
            discard(work);
        }
        finish();
    }
};
connection_output::connection_output(send_callback send, sent_callback sent,
    std::function<void()> closed, size_t limit) : state_(new state(
        std::move(send), std::move(sent), std::move(closed), limit)) {}
connection_output::~connection_output() { stop(); }
void connection_output::start() {
    if (!acl_fiber_running())
        throw std::logic_error("output writer requires an owning fiber");
    if (state_->worker)
        throw std::logic_error("output writer already started");
    state_->worker.reset(new state::writer(*state_));
    state_->worker->start(64000);
}
void connection_output::stop() {
    state_->finish();
    if (state_->worker) {
        {
            auto guard = state_->lock();
            while (!state_->finished) state_->delivered.wait(state_->mutex);
        }
        state_->worker.reset();
    }
}
connection_output::ticket connection_output::reserve_reply() {
    auto& output = *state_;
    try {
        auto guard = output.lock();
        if (output.ended) return 0;
        if (output.pending_id) return output.pending_id;
        if (output.next != UINT64_MAX &&
            output.fits(0, state::node_memory())) {
            output.queue.emplace_back(frame(), output.next + 1, false);
            output.pending_reply = std::prev(output.queue.end());
            output.pending_id = ++output.next;
            output.add(frame(), true);
            return output.pending_id;
        }
    } catch (const std::bad_alloc&) {}
    output.finish(); return 0;
}
bool connection_output::capture_external() {
    auto& output = *state_;
    auto guard = output.lock();
    if (output.ended || !output.pending_id) return false;
    auto& before = output.pending_reply->before;
    auto position = std::next(output.pending_reply);
    while (position != output.queue.end()) {
        auto current = position++;
        if (!current->self) before.splice(before.end(), output.queue, current);
    }
    return true;
}
bool connection_output::fulfil_reply(ticket reply, frame data) {
    auto& output = *state_;
    {
        auto guard = output.lock();
        if (output.ended || !reply || output.pending_id != reply || !data)
            return false;
        if (output.fits(data->size(), state::payload_memory(data))) {
            output.add(data, false);
            output.pending_reply->data = std::move(data);
            output.pending_id = 0; output.ready.notify(); return true;
        }
    }
    output.finish(); return false;
}
bool connection_output::append(frame data, bool self) {
    auto& output = *state_;
    if (!data) return false;
    try {
        auto guard = output.lock();
        if (output.ended) return false;
        if (output.fits(data->size(), state::node_memory() +
                state::payload_memory(data))) {
            output.queue.emplace_back(std::move(data), 0, self);
            output.add(output.queue.back().data, true);
            output.ready.notify(); return true;
        }
    } catch (const std::bad_alloc&) {}
    output.finish(); return false;
}
bool connection_output::wait(ticket reply) {
    auto& output = *state_;
    auto guard = output.lock();
    if (!reply || reply > output.next) return false;
    while (output.completed < reply && !output.ended) {
        if (acl::fiber::self_killed()) return false;
        output.delivered.wait(output.mutex);
    }
    return output.completed >= reply;
}
bool connection_output::limit(size_t bound) {
    auto& output = *state_;
    {
        auto guard = output.lock();
        if (output.ended) return false;
        if (output.bytes.load() <= bound && output.heap.load() <= bound) {
            output.limit = bound; return true;
        }
    }
    output.finish(); return false;
}
bool connection_output::closed() const { return state_->ended.load(); }
size_t connection_output::pending() const { return state_->bytes.load(); }
size_t connection_output::owned() const {
    return sizeof(*state_) + state_->heap.load();
}
size_t connection_output::frames() const { return state_->count.load(); }
} // namespace pkv
