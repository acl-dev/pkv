#pragma once
#include "client_activity.h"
#include <array>
#include <atomic>
#include <cstdint>
#include <string>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace pkv {
class client_state;
class blocked_client;
struct client_snapshot;
// A non-owning socket handle. Only the connection's owner closes the socket.
// Retire it before closing, even when snapshots still hold this object.
class client_connection {
public:
    enum class state { open, after_reply, immediate, retired };
    long long id() const { return id_; }
    state status() const { return state_.load(std::memory_order_acquire); }
    bool request_stop(bool after_reply = false);
    bool block(const std::shared_ptr<blocked_client>& value);
    void clear_block(const std::shared_ptr<blocked_client>& value);
    std::shared_ptr<blocked_client> blocked() const;
    bool unblock(bool error);
    bool read_metadata(client_snapshot& output) const;
    bool read_view(client_view& output, long long now_ms) const;
    const std::string& address() const { return address_; }
    const std::string& local_address() const { return local_address_; }
    long long age(long long now_seconds) const {
        return now_seconds - created_;
    }
private:
    friend class client_registry;
    client_connection(long long id, intptr_t socket,
        const client_state* metadata, client_inspector inspector);
    void retire();
    const long long id_;
    std::atomic<state> state_{state::open};
    intptr_t socket_;
    const long long created_;
    const std::string address_, local_address_;
    const client_state* metadata_;
    client_inspector inspector_;
    const bool unix_socket_;
    mutable std::mutex mutex_;
    std::shared_ptr<blocked_client> blocked_;
};

class client_registry {
public:
    using connection = std::shared_ptr<client_connection>;
    client_registry() = default;
    ~client_registry();
    client_registry(const client_registry&) = delete;
    client_registry& operator=(const client_registry&) = delete;
    connection add(long long id, intptr_t socket,
        const client_state* metadata = nullptr,
        client_inspector inspector = client_inspector());
    bool retire(const connection& entry);
    connection find(long long id) const;
    // Membership is captured together; state may change after locks release.
    // Ordering is optional; unsorted capture is linear and retains no socket.
    std::vector<connection> snapshot(bool ordered = true) const;
private:
    struct bucket {
        mutable std::mutex mutex;
        std::unordered_map<long long, connection> entries;
    };
    static constexpr size_t stripes = 64;
    mutable std::array<bucket, stripes> buckets_;
    bucket& stripe(long long id) const;
};
} // namespace pkv
