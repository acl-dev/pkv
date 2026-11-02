#include "transaction_queue.h"
#include "coder/redis_object.h"
#include <new>

namespace pkv {
bool transaction_queue::start() {
    if (active_) return false;
    active_ = true; failed_ = false; return true;
}
void transaction_queue::clear() {
    std::vector<std::vector<std::string>>().swap(commands_);
    bytes_ = 0; active_ = failed_ = false;
}
transaction_queue::result transaction_queue::append(
    const redis_object& object) {
    size_t charged = sizeof(std::vector<std::string>);
    if (commands_.size() >= max_commands_ || charged > max_bytes_ - bytes_)
        { failed_ = true; return result::full; }
    for (size_t i = 0; i < object.size(); ++i) {
        const auto* argument = object.argument(i);
        if (!argument || argument->size() > max_bytes_ - bytes_ - charged ||
            sizeof(std::string) > max_bytes_ - bytes_ - charged -
                argument->size()) {
            failed_ = true; return result::full;
        }
        charged += sizeof(std::string) + argument->size();
    }
    try {
        std::vector<std::string> arguments;
        arguments.reserve(object.size());
        for (size_t i = 0; i < object.size(); ++i)
            arguments.push_back(*object.argument(i));
        commands_.push_back(std::move(arguments));
    } catch (const std::bad_alloc&) { failed_ = true; throw; }
    bytes_ += charged;
    return result::queued;
}
std::vector<std::vector<std::string>> transaction_queue::take() {
    std::vector<std::vector<std::string>> result;
    result.swap(commands_); clear(); return result;
}
} // namespace pkv
