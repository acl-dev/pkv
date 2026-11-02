#include "db.h"
#include "common/key_wait.h"
#include <stdexcept>

namespace pkv {
std::shared_ptr<key_wait_subscription> db::wait_for_keys(
    const std::vector<std::string>& keys) {
    // A workspace must not register a wait on its private uncommitted state.
    if (!wait_registry_)
        throw std::logic_error("actor-local DB cannot register a wait");
    return wait_registry_->subscribe(keys);
}
void db::invalidate_watch(const std::string& key, bool erasing) {
    if (watch_registry_) watch_registry_->invalidate(key, erasing);
    if (wait_registry_) wait_registry_->signal(key);
}
} // namespace pkv
