#include "key_wait.h"
#include "key_wait_batch.h"
#include <acl-lib/fiber/fiber.hpp>
#include <acl-lib/fiber/fiber_mutex.hpp>
#include <chrono>
#include <algorithm>
#include <array>
#include <cstdlib>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace pkv {
static std::atomic<size_t> type_waiters[
    static_cast<size_t>(key_wait_kind::count)]{};
size_t key_wait_registry::waiting(key_wait_kind kind) {
    if (kind < key_wait_kind::generic || kind >= key_wait_kind::count) return 0;
    return type_waiters[static_cast<size_t>(kind)].load();
}
unsigned key_wait_registry::waiting_types() {
    unsigned mask = 0;
    for (unsigned i = 1; i < static_cast<unsigned>(key_wait_kind::count); ++i)
        if (type_waiters[i].load()) mask |= 1U << i;
    return mask;
}
struct key_wait_entry {
    key_wait_node* head = nullptr;
    key_wait_node* tail = nullptr;
    std::array<key_wait_node*,
        static_cast<size_t>(key_wait_kind::count)> kind_heads{}, kind_tails{};
    key_wait_subscription* reserved = nullptr;
    uint64_t generation = 0, order = 0;
    long long deadline = -1;
    bool ready = false, probe = false;
    key_wait_kind kind = key_wait_kind::generic;
};
struct key_wait_node {
    const std::string key;
    key_wait_subscription& owner;
    const size_t slot;
    const key_wait_kind kind;
    key_wait_node* promoted = nullptr;
    uint64_t priority = 0;
    key_wait_entry* entry = nullptr;
    key_wait_node* previous = nullptr;
    key_wait_node* next = nullptr;
    key_wait_node* kind_previous = nullptr;
    key_wait_node* kind_next = nullptr;
    key_wait_node(const std::string& name, key_wait_subscription& token,
        size_t index, key_wait_kind type)
        : key(name), owner(token), slot(index),
        kind(type) {}
};
struct key_wait_registry::state {
    acl::fiber_mutex mutex;
    std::unordered_map<std::string, key_wait_entry> entries;
    uint64_t order = 0;
    key_wait_subscription* head = nullptr;
    key_wait_subscription* tail = nullptr;
    bool dispatching = false;
    uint64_t publication = 0, priority_epoch = 0;
    std::unique_lock<acl::fiber_mutex> lock() {
        if (!mutex.lock()) std::abort();
        return std::unique_lock<acl::fiber_mutex>(mutex, std::adopt_lock);
    }
};
static long long wait_now() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}
static key_wait_node* kind_head(key_wait_entry& entry, key_wait_kind kind) {
    if (kind < key_wait_kind::generic || kind >= key_wait_kind::count)
        return nullptr;
    return entry.kind_heads[static_cast<size_t>(kind)];
}
static bool actual(const key_wait_node* node, long long now) {
    return node && node->entry && node->entry->ready &&
        node->entry->kind == node->kind && (node->entry->deadline < 0 ||
            node->entry->deadline > now);
}
template<class Callback>
static void each_head(key_wait_entry& entry, const Callback& callback) {
    for (auto* head : entry.kind_heads)
        if (head) callback(head);
}
key_wait_registry::key_wait_registry(bool readiness)
    : state_(new state), readiness_(readiness) {}
key_wait_registry::~key_wait_registry() = default;
key_wait_subscription::key_wait_subscription(
    std::shared_ptr<key_wait_registry> owner,
    const std::vector<std::string>& keys, key_wait_kind kind)
    : owner_(std::move(owner)), kind_(kind) {
    if (keys.empty()) throw std::invalid_argument("empty wait key set");
    std::unordered_set<std::string> seen;
    for (const auto& key : keys) {
        if (!seen.insert(key).second) continue;
        std::unique_ptr<key_wait_node> node(new key_wait_node(key, *this,
            nodes_.size(), kind));
        nodes_.push_back(std::move(node));
    }
    work_.reserve(nodes_.size());
    for (auto& node : nodes_) work_.push_back(node.get());
}
key_wait_subscription::~key_wait_subscription() {
    if (armed_) owner_->remove(*this);
}
bool key_wait_subscription::wait(int timeout_ms, key_wait_event& event) {
    return owner_->wait(*this, timeout_ms, event);
}
bool key_wait_subscription::first(const std::string& key) const {
    return owner_->first(*this, key);
}
bool key_wait_registry::first(const key_wait_subscription& token,
    const std::string& key) {
    auto lock = state_->lock();
    const auto found = state_->entries.find(key);
    if (found == state_->entries.end()) return false;
    const auto* head = kind_head(found->second, token.kind_);
    return head && &head->owner == &token && (!found->second.reserved ||
        found->second.reserved == &token);
}
const std::string& key_wait_subscription::key(
    const key_wait_event& event) const {
    if (event.source != this) throw std::logic_error("foreign wait turn");
    return nodes_.at(event.slot)->key;
}
void key_wait_subscription::miss(const key_wait_event& event) {
    owner_->miss(*this, event);
}
bool key_wait_subscription::refresh(key_wait_event& event) {
    return owner_->refresh(*this, event);
}
void key_wait_subscription::interrupt() noexcept { owner_->interrupt(*this); }
key_wait_token key_wait_registry::subscribe(
    const std::vector<std::string>& keys, key_wait_kind kind) {
    if (kind < key_wait_kind::generic || kind >= key_wait_kind::count)
        throw std::invalid_argument("invalid wait kind");
    key_wait_token token(new key_wait_subscription(shared_from_this(), keys,
        kind));
    auto lock = state_->lock();
    try {
        for (auto& node : token->nodes_) {
            auto found = state_->entries.find(node->key);
            if (found == state_->entries.end())
                found = state_->entries.emplace(node->key,
                    key_wait_entry()).first;
            auto& entry = found->second;
            node->entry = &entry; node->previous = entry.tail;
            if (entry.tail) entry.tail->next = node.get();
            else entry.head = node.get();
            entry.tail = node.get();
            const auto index = static_cast<size_t>(kind);
            node->kind_previous = entry.kind_tails[index];
            if (node->kind_previous)
                node->kind_previous->kind_next = node.get();
            else entry.kind_heads[index] = node.get();
            entry.kind_tails[index] = node.get();
        }
    } catch (...) { unlink(*token); throw; }
    token->armed_ = true;
    active_.fetch_add(1, std::memory_order_release);
    type_waiters[static_cast<size_t>(kind)].fetch_add(1);
    schedule(*token);
    return token;
}
key_wait_node* key_wait_registry::select(
    key_wait_subscription& token) noexcept {
    key_wait_node* first = nullptr;
    const auto now = wait_now();
    if (readiness_)
        for (const auto& node : token.nodes_)
            if (node->entry && node->entry->reserved &&
                node->entry->reserved != &token) return nullptr;
    for (auto& node : token.nodes_) {
        auto* entry = node->entry;
        if (actual(node.get(), now) &&
            kind_head(*entry, token.kind_) == node.get() &&
            (!first || entry->order < first->entry->order)) first = node.get();
    }
    if (!first && readiness_)
        for (auto& node : token.nodes_)
            if (node->entry && node->entry->probe &&
                kind_head(*node->entry, token.kind_) == node.get())
                return node.get();
    return first;
}
void key_wait_registry::schedule(key_wait_subscription& token) noexcept {
    enqueue(token); dispatch();
}
void key_wait_registry::enqueue(key_wait_subscription& token) noexcept {
    if (token.queued_ || !token.armed_) return;
    token.queued_ = true; token.next_ = nullptr;
    if (state_->tail) state_->tail->next_ = &token;
    else state_->head = &token;
    state_->tail = &token;
}
void key_wait_registry::dispatch() noexcept {
    if (state_->dispatching) return;
    state_->dispatching = true;
    while (state_->head) {
        auto& token = *state_->head;
        state_->head = token.next_;
        if (!state_->head) state_->tail = nullptr;
        token.queued_ = false; token.next_ = nullptr;
        update(token);
    }
    state_->dispatching = false;
}
void key_wait_registry::reserve(key_wait_subscription& token) noexcept {
    for (auto& node : token.nodes_) node->entry->reserved = &token;
    token.reserved_ = true;
}
void key_wait_registry::release(key_wait_subscription& token) noexcept {
    if (!token.reserved_) return;
    for (auto& node : token.nodes_)
        if (node->entry && node->entry->reserved == &token)
            node->entry->reserved = nullptr;
    token.reserved_ = false;
}
void key_wait_registry::heads(key_wait_subscription& token) noexcept {
    const auto epoch = ++state_->priority_epoch;
    const auto now = wait_now();
    for (auto* node : token.work_) {
        auto* entry = node->entry;
        node->priority = UINT64_MAX; node->promoted = nullptr;
        if (!entry) continue;
        each_head(*entry, [&](key_wait_node* candidate) {
            auto& owner = candidate->owner;
            if (owner.priority_epoch_ != epoch) {
                auto* best = select(owner);
                owner.priority_epoch_ = epoch;
                owner.priority_ = best ? actual(best, now) ?
                    best->entry->order : UINT64_MAX - 1 : UINT64_MAX;
            }
            if (!node->promoted || owner.priority_ < node->priority) {
                node->promoted = candidate; node->priority = owner.priority_;
            }
        });
    }
    std::sort(token.work_.begin(), token.work_.end(),
        [](const key_wait_node* a, const key_wait_node* b) {
        return a->priority < b->priority;
    });
    for (auto* node : token.work_) {
        if (!node->entry) continue;
        if (node->promoted) enqueue(node->promoted->owner);
        each_head(*node->entry, [&](key_wait_node* head) {
            enqueue(head->owner);
        });
    }
}
void key_wait_registry::update(key_wait_subscription& token) noexcept {
    if (!readiness_) {
        if (token.claimed_ || token.pending_) return;
        token.pending_ = select(token);
    } else {
        if (token.reserved_ && token.pending_ &&
            actual(token.pending_, wait_now())) return;
        auto* best = select(token);
        const bool ready = actual(best, wait_now());
        if (ready && !token.reserved_) reserve(token);
        if (token.claimed_) return;
        if (!ready && token.reserved_) { release(token); heads(token); }
        token.pending_ = best;
    }
    if (token.pending_) token.changed_.notify();
}
void key_wait_registry::signal(const std::string& key) noexcept {
    if (!active_.load(std::memory_order_acquire)) return;
    auto lock = state_->lock();
    const auto found = state_->entries.find(key);
    if (found == state_->entries.end()) return;
    auto& entry = found->second;
    ++entry.generation;
    if (!entry.ready) { entry.ready = true; entry.order = ++state_->order; }
    auto& token = entry.head->owner;
    if (readiness_) { schedule(token); return; }
    if (!token.pending_) {
        token.pending_ = entry.head;
        token.changed_.notify();
    }
}
void key_wait_registry::prepare(const key_ready_change* changes,
    size_t count) {
    key_wait_batch::prepare(*this, changes, count);
}
void key_wait_registry::publish(
    const std::vector<key_ready_change>& changes) noexcept {
    publish(changes.data(), changes.size());
}
void key_wait_registry::publish(const key_ready_change* changes,
    size_t count) noexcept {
    if (key_wait_batch::record(*this, changes, count)) return;
    if (!active_.load(std::memory_order_acquire)) return;
    auto lock = state_->lock();
    const auto now = wait_now();
    const auto epoch = ++state_->publication;
    for (size_t i = 0; i < count; ++i) {
        const auto& change = changes[i];
        if (!change.key) continue;
        const auto found = state_->entries.find(*change.key);
        if (found == state_->entries.end()) continue;
        auto& entry = found->second;
        ++entry.generation;
        if (change.ready && (!entry.ready || entry.kind != change.kind ||
            (entry.deadline >= 0 && entry.deadline <= now)))
            entry.order = ++state_->order;
        entry.ready = change.ready; entry.kind = change.kind;
        entry.probe = false;
        entry.deadline = change.deadline;
    }
    for (size_t i = 0; i < count; ++i) {
        const auto& change = changes[i];
        if (!change.key) continue;
        const auto found = state_->entries.find(*change.key);
        if (found == state_->entries.end()) continue;
        auto& entry = found->second;
        const auto offer = [&](key_wait_node* node) {
            if (!node) return;
            auto& owner = node->owner;
            if (owner.publication_ == epoch) return;
            owner.publication_ = epoch;
            schedule(owner);
        };
        offer(kind_head(entry, entry.kind));
        each_head(entry, offer);
    }
}
void key_wait_registry::unlink(key_wait_subscription& token) noexcept {
    release(token);
    // Detach every node before promoting another multi-key subscriber.
    for (auto& node : token.nodes_) {
        auto* entry = node->entry;
        if (!entry) continue;
        const auto index = static_cast<size_t>(node->kind);
        const bool first = entry->kind_heads[index] == node.get();
        if (node->kind_previous)
            node->kind_previous->kind_next = node->kind_next;
        else entry->kind_heads[index] = node->kind_next;
        if (node->kind_next)
            node->kind_next->kind_previous = node->kind_previous;
        else entry->kind_tails[index] = node->kind_previous;
        if (node->previous) node->previous->next = node->next;
        else {
            entry->head = node->next;
        }
        if (first && entry->head && (readiness_ || !entry->ready)) {
            // Promote each type's head without giving a probe producer rank.
            if (readiness_) entry->probe = true;
            else { entry->ready = true; entry->order = ++state_->order; }
            ++entry->generation;
        }
        if (node->next) node->next->previous = node->previous;
        else entry->tail = node->previous;
    }
    heads(token);
    for (auto& node : token.nodes_) {
        auto* entry = node->entry;
        if (!entry) continue;
        if (!entry->head) state_->entries.erase(node->key);
        node->entry = nullptr;
    }
    dispatch();
}
void key_wait_registry::remove(key_wait_subscription& token) noexcept {
    auto lock = state_->lock();
    unlink(token);
    token.armed_ = false;
    active_.fetch_sub(1, std::memory_order_release);
    type_waiters[static_cast<size_t>(token.kind_)].fetch_sub(1);
}
bool key_wait_registry::wait(key_wait_subscription& token, int timeout_ms,
    key_wait_event& event) {
    using clock = std::chrono::steady_clock;
    const auto deadline = clock::now() + std::chrono::milliseconds(
        timeout_ms < 0 ? 0 : timeout_ms);
    auto lock = state_->lock();
    if (token.interrupted_) return false;
    if (token.claimed_) throw std::logic_error("wait turn already claimed");
    while (!token.pending_ && !token.interrupted_) {
        if (acl::fiber::self_killed()) return false;
        int remaining = -1;
        if (timeout_ms >= 0) {
            const auto left = deadline - clock::now();
            if (left <= clock::duration::zero()) return false;
            remaining = static_cast<int>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    left).count());
            if (!remaining) remaining = 1;
        }
        token.changed_.wait(state_->mutex, remaining);
    }
    if (token.interrupted_ || acl::fiber::self_killed()) return false;
    event.slot = token.pending_->slot;
    event.source = &token;
    event.generation = token.pending_->entry->generation;
    token.claimed_generation_ = event.generation;
    event.ready = !readiness_ || actual(token.pending_, wait_now());
    token.claimed_ = true;
    return true;
}
void key_wait_registry::miss(key_wait_subscription& token,
    const key_wait_event& event) {
    auto lock = state_->lock();
    if (token.interrupted_) return;
    auto* node = token.pending_;
    if (event.source != &token || !token.claimed_ || !node ||
        node->slot != event.slot ||
        token.claimed_generation_ != event.generation)
        throw std::logic_error("invalid wait turn acknowledgement");
    if (node->entry->generation == event.generation) {
        if (!readiness_ || event.ready) node->entry->ready = false;
        node->entry->probe = false;
    }
    token.pending_ = nullptr; token.claimed_ = false;
    schedule(token);
}
bool key_wait_registry::refresh(key_wait_subscription& token,
    key_wait_event& event) {
    auto lock = state_->lock();
    if (token.interrupted_) { event.source = nullptr; return false; }
    token.pending_ = select(token);
    bool ready = actual(token.pending_, wait_now());
    if (readiness_ && !ready && token.reserved_) {
        release(token); heads(token); dispatch();
        token.pending_ = select(token);
        ready = actual(token.pending_, wait_now());
    }
    token.claimed_ = token.pending_ != nullptr;
    if (!token.pending_) { event.source = nullptr; return false; }
    if (readiness_ && ready && !token.reserved_) reserve(token);
    event.source = &token; event.slot = token.pending_->slot;
    event.generation = token.pending_->entry->generation;
    token.claimed_generation_ = event.generation;
    event.ready = !readiness_ || ready;
    return true;
}
void key_wait_registry::interrupt(key_wait_subscription& token) noexcept {
    auto lock = state_->lock();
    token.interrupted_ = true;
    if (readiness_ && token.armed_) {
        unlink(token); token.armed_ = false;
        active_.fetch_sub(1, std::memory_order_release);
        type_waiters[static_cast<size_t>(token.kind_)].fetch_sub(1);
    }
    token.changed_.notify();
}
} // namespace pkv
