#include "key_wait_batch.h"
#include <acl-lib/fiber/lib_fiber.h>
#include <algorithm>
#include <cstdlib>
#include <map>
#include <stdexcept>

namespace pkv {
namespace {
thread_local key_wait_batch* active = nullptr;
unsigned actor() { return acl_fiber_running() ? acl_fiber_self() : 0; }
struct ready_slot {
    key_ready_change value{nullptr, false, -1};
    uint64_t order = UINT64_MAX;
    unsigned pending_types = 0;
    bool committed = false;
};
struct ready_group {
    explicit ready_group(std::shared_ptr<key_wait_registry> source)
        : registry(std::move(source)) {}
    std::shared_ptr<key_wait_registry> registry;
    std::map<std::string, ready_slot> slots;
    std::vector<ready_slot*> order;
    std::vector<key_ready_change> changes;
    uint64_t next = 0;
    void prepare(const key_ready_change* incoming, size_t count) {
        if (count > order.max_size() - order.size() ||
            count > changes.max_size() - order.size()) throw std::bad_alloc();
        order.reserve(order.size() + count);
        changes.reserve(order.size() + count);
        const auto types = key_wait_registry::waiting_types();
        for (size_t i = 0; i < count; ++i) {
            if (!incoming[i].key) continue;
            auto found = slots.find(*incoming[i].key);
            if (found == slots.end()) {
                found = slots.emplace(*incoming[i].key, ready_slot()).first;
                found->second.value.key = &found->first;
                order.push_back(&found->second);
            }
            found->second.pending_types = types;
        }
    }
    void record(const key_ready_change* incoming, size_t count) noexcept {
        for (size_t i = 0; i < count; ++i) {
            const auto& change = incoming[i];
            if (!change.key) continue;
            const auto found = slots.find(*change.key);
            if (found == slots.end()) std::abort();
            auto& slot = found->second;
            if (slot.order == UINT64_MAX && change.ready &&
                (slot.pending_types &
                    (1U << static_cast<unsigned>(change.kind))))
                slot.order = next++;
            slot.committed = true;
            slot.value = change; slot.value.key = &found->first;
        }
    }
    void publish() noexcept {
        std::sort(order.begin(), order.end(),
            [](const ready_slot* a, const ready_slot* b) {
            return a->order < b->order;
        });
        for (const auto* slot : order)
            if (slot->committed) changes.push_back(slot->value);
        registry->publish(changes);
    }
};
}
struct key_wait_batch::state {
    unsigned owner = actor();
    std::map<key_wait_registry*, std::unique_ptr<ready_group>> groups;
};
key_wait_batch::key_wait_batch() : state_(new state) {
    if (active) throw std::logic_error("nested key readiness batch");
    active = this;
}
key_wait_batch::~key_wait_batch() {
    if (active != this || state_->owner != actor()) std::abort();
    active = nullptr;
    for (auto& group : state_->groups) group.second->publish();
}
key_wait_batch* key_wait_batch::current() noexcept {
    return active && active->state_->owner == actor() ? active : nullptr;
}
void key_wait_batch::prepare(key_wait_registry& registry,
    const key_ready_change* changes, size_t count) {
    auto* batch = current();
    if (!batch) return;
    bool found_key = false;
    for (size_t i = 0; i < count; ++i) found_key |= changes[i].key != nullptr;
    if (!found_key) return;
    auto& groups = batch->state_->groups;
    auto found = groups.find(&registry);
    if (found == groups.end()) {
        std::unique_ptr<ready_group> group(new ready_group(
            registry.shared_from_this()));
        found = groups.emplace(&registry, std::move(group)).first;
    }
    found->second->prepare(changes, count);
}
bool key_wait_batch::record(key_wait_registry& registry,
    const key_ready_change* changes, size_t count) noexcept {
    auto* batch = current();
    if (!batch) return false;
    const auto found = batch->state_->groups.find(&registry);
    if (found != batch->state_->groups.end())
        found->second->record(changes, count);
    else for (size_t i = 0; i < count; ++i)
        if (changes[i].key) std::abort();
    return true;
}
} // namespace pkv
