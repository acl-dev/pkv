#include "client_state.h"
#include "client_memory.h"
#include <atomic>
#include <climits>
#include <cstdint>
#include <cstring>
#include <new>
#include <stdexcept>
#include <strings.h>

namespace pkv {
namespace {
long long next_id() {
    static std::atomic<uint64_t> next(1);
    auto value = next.load(std::memory_order_relaxed);
    for (;;) {
        if (value > static_cast<uint64_t>(LLONG_MAX))
            throw std::overflow_error("client identifiers exhausted");
        if (next.compare_exchange_weak(value, value + 1,
            std::memory_order_relaxed)) return static_cast<long long>(value);
    }
}
} // namespace
const client_command_spec* client_subcommand(const std::string& name) {
    static const client_command_spec commands[] = {
        {"id", 2, "client|id"}, {"getname", 2, "client|getname"},
        {"setname", 3, "client|setname"}, {"setinfo", 4, "client|setinfo"},
        {"kill", -3, "client|kill"}, {"help", 2, "client|help"},
        {"info", 2, "client|info"}, {"list", -2, "client|list"},
        {"unblock", -3, "client|unblock"}
    };
    for (const auto& spec : commands)
        if (name.size() == std::strlen(spec.name) &&
            !strncasecmp(name.data(), spec.name, name.size())) return &spec;
    return nullptr;
}
client_state::client_state() : id_(next_id()) { (void) username(); }
const std::string& client_state::username() const {
    static const std::string default_name("default");
    return username_ ? *username_ : default_name;
}
client_state::status client_state::login(const std::string& user,
    uint64_t user_id) {
    try {
        std::shared_ptr<const std::string> staged;
        if (user != "default") staged = std::make_shared<const std::string>(
            copy_client_string(user));
        std::lock_guard<std::mutex> lock(mutex_);
        username_ = std::move(staged); user_id_ = user_id;
        authenticated_ = true;
        return status::ok;
    } catch (const std::bad_alloc&) { return status::no_memory; }
}
bool client_state::valid_metadata(const std::string& value) {
    for (const unsigned char ch : value)
        if (ch < '!' || ch > '~') return false;
    return true;
}
client_state::status client_state::assign(std::string& target,
    const std::string& value) {
    if (!valid_metadata(value)) return status::invalid;
    try {
        auto owned = copy_client_string(value);
        std::lock_guard<std::mutex> lock(mutex_);
        target.swap(owned);
        return status::ok;
    } catch (const std::bad_alloc&) { return status::no_memory; }
}
client_snapshot client_state::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    client_snapshot result;
    result.id = id_; result.protocol = protocol_;
    result.user_id = user_id_;
    result.username = copy_client_string(username());
    result.name = copy_client_string(name_);
    result.library_name = copy_client_string(library_name_);
    result.library_version = copy_client_string(library_version_);
    result.heap_bytes = string_heap_bytes(name_) +
        string_heap_bytes(library_name_) + string_heap_bytes(library_version_);
    if (username_) result.heap_bytes += sizeof(std::string) +
        string_heap_bytes(*username_);
    return result;
}
client_state::status client_state::hello(unsigned protocol,
    const std::string* name, const std::string* user, uint64_t user_id) {
    if (protocol < 2 || protocol > 3 || (name && !valid_metadata(*name)))
        return status::invalid;
    try {
        std::shared_ptr<const std::string> staged;
        if (user && *user != "default")
            staged = std::make_shared<const std::string>(
                copy_client_string(*user));
        std::string owned;
        if (name) owned = copy_client_string(*name);
        std::lock_guard<std::mutex> lock(mutex_);
        if (name) name_.swap(owned);
        if (user) { username_ = std::move(staged); user_id_ = user_id; }
        protocol_ = protocol;
        authenticated_ = true;
        return status::ok;
    } catch (const std::bad_alloc&) { return status::no_memory; }
}
void client_state::reset(bool authenticated) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    authenticated_ = authenticated;
    protocol_ = 2;
    username_.reset(); user_id_ = 1;
    std::string().swap(name_);
}
} // namespace pkv
