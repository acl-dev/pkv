#pragma once
#include <acl-lib/fiber/fiber_cond.hpp>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace pkv {
class key_wait_registry;
class key_wait_subscription;
struct key_wait_node;
struct key_wait_event {
    const key_wait_subscription* source = nullptr;
    size_t slot = SIZE_MAX;
    uint64_t generation = 0;
    bool ready = false;
};
enum class key_wait_kind { generic, list, zset, count };
struct key_ready_change {
    key_ready_change(const std::string* name, bool available, long long expiry,
        key_wait_kind type = key_wait_kind::generic)
        : key(name), ready(available), deadline(expiry), kind(type) {}
    const std::string* key;
    bool ready;
    long long deadline;
    key_wait_kind kind;
};
// A readiness hint reserves the head's turn; it does not reserve a value.
// Probe storage without the registry mutex. On a miss, acknowledge the exact
// generation; on success/timeout/disconnect destroy the subscription.
class key_wait_subscription {
public:
    ~key_wait_subscription();
    bool wait(int timeout_ms, key_wait_event& event);
    // Used inside the initial storage probe: do not consume ahead of an
    // older subscriber. The probe must still hold the DB's ordinary key locks.
    bool first(const std::string& key) const;
    const std::string& key(const key_wait_event& event) const;
    void miss(const key_wait_event& event);
    // Re-select after admission and all candidate key locks are acquired.
    bool refresh(key_wait_event& event);
    void interrupt() noexcept;
    key_wait_subscription(const key_wait_subscription&) = delete;
    key_wait_subscription& operator=(const key_wait_subscription&) = delete;
private:
    friend class key_wait_registry;
    key_wait_subscription(std::shared_ptr<key_wait_registry> owner,
        const std::vector<std::string>& keys, key_wait_kind kind);
    std::shared_ptr<key_wait_registry> owner_;
    const key_wait_kind kind_;
    std::vector<std::unique_ptr<key_wait_node>> nodes_;
    std::vector<key_wait_node*> work_;
    acl::fiber_cond changed_;
    key_wait_node* pending_ = nullptr;
    uint64_t claimed_generation_ = 0;
    bool claimed_ = false, armed_ = false;
    bool interrupted_ = false;
    bool reserved_ = false, queued_ = false;
    key_wait_subscription* next_ = nullptr;
    uint64_t publication_ = 0, priority_epoch_ = 0, priority_ = 0;
};
using key_wait_token = std::shared_ptr<key_wait_subscription>;

// Shared by all workers serving one physical logical DB. Short fiber-aware
// locks only manage queues; they never access storage or execute callbacks.
// Subscribe before checking storage to close the empty-probe/register race.
// Publication after a successful commit performs no C++ allocation.
class key_wait_registry :
    public std::enable_shared_from_this<key_wait_registry> {
public:
    explicit key_wait_registry(bool readiness = false);
    ~key_wait_registry();
    key_wait_token subscribe(const std::vector<std::string>& keys,
        key_wait_kind kind = key_wait_kind::generic);
    void signal(const std::string& key) noexcept;
    void prepare(const key_ready_change* changes, size_t count);
    void publish(const std::vector<key_ready_change>& changes) noexcept;
    void publish(const key_ready_change* changes, size_t count) noexcept;
    size_t subscribers() const { return active_.load(); }
    static size_t waiting(key_wait_kind kind);
    static unsigned waiting_types();
private:
    friend class key_wait_subscription;
    struct state;
    std::unique_ptr<state> state_;
    std::atomic<size_t> active_{0};
    const bool readiness_;
    void remove(key_wait_subscription& token) noexcept;
    void unlink(key_wait_subscription& token) noexcept;
    void schedule(key_wait_subscription& token) noexcept;
    bool wait(key_wait_subscription& token, int timeout_ms,
        key_wait_event& event);
    void miss(key_wait_subscription& token, const key_wait_event& event);
    bool first(const key_wait_subscription& token, const std::string& key);
    key_wait_node* select(key_wait_subscription& token) noexcept;
    bool refresh(key_wait_subscription& token, key_wait_event& event);
    void interrupt(key_wait_subscription& token) noexcept;
    void enqueue(key_wait_subscription& token) noexcept;
    void dispatch() noexcept;
    void update(key_wait_subscription& token) noexcept;
    void heads(key_wait_subscription& token) noexcept;
    void release(key_wait_subscription& token) noexcept;
    void reserve(key_wait_subscription& token) noexcept;
};
} // namespace pkv
