#include "client_registry.h"
#include "blocked_client.h"
#include "client_state.h"
#include "client_memory.h"
#include <algorithm>
#include <chrono>
#include <stdexcept>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <sys/un.h>
#include <netdb.h>
#include <cstddef>
#include <cstring>
#endif

namespace pkv {
namespace {
bool unix_socket(intptr_t socket) {
#ifdef _WIN32
    (void) socket; return false;
#else
    sockaddr_storage address{};
    socklen_t size = sizeof(address);
    return !::getsockname(static_cast<int>(socket),
        reinterpret_cast<sockaddr*>(&address), &size) &&
        address.ss_family == AF_UNIX;
#endif
}
std::string socket_address(intptr_t socket, bool local) {
#ifdef _WIN32
    const auto fd = static_cast<SOCKET>(socket);
    using address_length = int;
#else
    const auto fd = static_cast<int>(socket);
    using address_length = socklen_t;
#endif
    sockaddr_storage address{};
    address_length length = sizeof(address);
    const int status = local ?
        ::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) :
        ::getpeername(fd, reinterpret_cast<sockaddr*>(&address), &length);
    if (status) return "?:0";
#ifndef _WIN32
    if (address.ss_family == AF_UNIX) {
        sockaddr_un name{};
        length = sizeof(name);
        if (::getsockname(fd, reinterpret_cast<sockaddr*>(&name), &length))
            return "?:0";
        const auto offset = offsetof(sockaddr_un, sun_path);
        if (length <= offset || !name.sun_path[0]) return "?:0";
        const auto size = std::min(sizeof(name.sun_path), length - offset);
        return std::string(name.sun_path, strnlen(name.sun_path, size)) + ":0";
    }
#endif
    char host[NI_MAXHOST], port[NI_MAXSERV];
    if (::getnameinfo(reinterpret_cast<sockaddr*>(&address), length,
        host, sizeof(host), port, sizeof(port),
        NI_NUMERICHOST | NI_NUMERICSERV)) return "?:0";
    return address.ss_family == AF_INET6 ?
        "[" + std::string(host) + "]:" + port :
        std::string(host) + ":" + port;
}
} // namespace
client_connection::client_connection(long long id, intptr_t socket,
    const client_state* metadata, client_inspector inspector)
: id_(id), socket_(socket), created_(
    std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count()),
  address_(socket_address(socket, false)),
  local_address_(socket_address(socket, true)), metadata_(metadata),
  inspector_(inspector), unix_socket_(unix_socket(socket)) {}

bool client_connection::read_metadata(client_snapshot& output) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!metadata_) return false;
    auto staged = metadata_->snapshot();
    output = std::move(staged);
    return true;
}
bool client_connection::read_view(client_view& output, long long now_ms) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!metadata_ || !inspector_.read) return false;
    client_view staged;
    staged.metadata = metadata_->snapshot();
    staged.runtime = inspector_.read(inspector_.context, now_ms);
    staged.runtime.blocked = blocked_ && blocked_->waiting();
    staged.runtime.owned_memory += sizeof(*this) + staged.metadata.heap_bytes +
        string_heap_bytes(address_) + string_heap_bytes(local_address_);
    staged.descriptor = socket_;
    staged.closing = static_cast<unsigned>(state_.load());
    staged.unix_socket = unix_socket_;
    output = std::move(staged);
    return true;
}
bool client_connection::request_stop(bool after_reply) {
    std::shared_ptr<blocked_client> blocked;
    bool removed;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto old = state_.load(std::memory_order_relaxed);
        if (old == state::retired || old == state::immediate) return false;
        if (old == state::after_reply && after_reply) return false;
        const auto next = after_reply ? state::after_reply : state::immediate;
        state_.store(next, std::memory_order_release);
        if (!after_reply) {
#ifdef _WIN32
            ::shutdown(static_cast<SOCKET>(socket_), SD_BOTH);
#else
            ::shutdown(static_cast<int>(socket_), SHUT_RDWR);
#endif
        }
        // Escalating a pending self-close does not count as a second removal.
        removed = old == state::open;
        blocked = blocked_;
    }
    if (!after_reply && blocked) blocked->disconnect();
    return removed;
}
bool client_connection::block(const std::shared_ptr<blocked_client>& value) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_.load() != state::open) return false;
    blocked_ = value; return true;
}
void client_connection::clear_block(
    const std::shared_ptr<blocked_client>& value) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (blocked_ == value) blocked_.reset();
}
std::shared_ptr<blocked_client> client_connection::blocked() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return blocked_;
}
bool client_connection::unblock(bool error) {
    const auto value = blocked();
    return value && value->cancel(error);
}
void client_connection::retire() {
    std::lock_guard<std::mutex> lock(mutex_);
    socket_ = -1;
    metadata_ = nullptr;
    inspector_ = client_inspector();
    state_.store(state::retired, std::memory_order_release);
}
client_registry::bucket& client_registry::stripe(long long id) const {
    return buckets_[static_cast<uint64_t>(id) % stripes];
}
client_registry::connection client_registry::add(long long id,
    intptr_t socket, const client_state* metadata, client_inspector inspector) {
#ifdef _WIN32
    const bool invalid = socket == -1;
#else
    const bool invalid = socket < 0;
#endif
    if (id <= 0 || invalid) throw std::invalid_argument("invalid client");
    if (metadata && metadata->id() != id)
        throw std::invalid_argument("mismatched client metadata identifier");
    // Construct before publishing: allocation failure leaves no live entry.
    connection entry(new client_connection(id, socket, metadata, inspector));
    auto& bucket = stripe(id);
    std::lock_guard<std::mutex> lock(bucket.mutex);
    if (!bucket.entries.emplace(id, entry).second)
        throw std::logic_error("duplicate client identifier");
    return entry;
}
bool client_registry::retire(const connection& entry) {
    if (!entry) return false;
    auto& bucket = stripe(entry->id());
    std::lock_guard<std::mutex> lock(bucket.mutex);
    const auto found = bucket.entries.find(entry->id());
    if (found == bucket.entries.end() || found->second != entry) return false;
    // This lock order is always bucket -> connection. Never close here.
    entry->retire();
    bucket.entries.erase(found);
    return true;
}
client_registry::connection client_registry::find(long long id) const {
    auto& bucket = stripe(id);
    std::lock_guard<std::mutex> lock(bucket.mutex);
    const auto found = bucket.entries.find(id);
    return found == bucket.entries.end() ? connection() : found->second;
}
std::vector<client_registry::connection> client_registry::snapshot(
    bool ordered) const {
    std::vector<connection> result;
    {
        // Other operations acquire at most one bucket. Snapshots acquire all
        // buckets in this fixed order and never perform network IO here.
        std::array<std::unique_lock<std::mutex>, stripes> locks;
        size_t count = 0;
        for (size_t i = 0; i < stripes; ++i) {
            locks[i] = std::unique_lock<std::mutex>(buckets_[i].mutex);
            count += buckets_[i].entries.size();
        }
        result.reserve(count);
        for (const auto& bucket : buckets_)
            for (const auto& item : bucket.entries)
                result.push_back(item.second);
    }
    if (ordered) std::sort(result.begin(), result.end(),
        [](const connection& a, const connection& b) {
            return a->id() < b->id();
        });
    return result;
}
client_registry::~client_registry() {
    // The registry must outlive its handlers. Invalidate any retained snapshot
    // before destroying the membership maps, without taking socket ownership.
    for (auto& bucket : buckets_)
        for (const auto& item : bucket.entries) item.second->retire();
}
} // namespace pkv
