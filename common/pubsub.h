#pragma once
#include "connection_output.h"
#include "pubsub_frame.h"
#include <array>
#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <vector>
namespace pkv {
class acl_identity;
struct pubsub_counts {
    size_t channel = 0, pattern = 0, shard = 0;
    size_t total() const { return channel + pattern + shard; }
};
class pubsub_subscriber {
public:
    pubsub_counts counts() const;
    size_t owned() const { return owned_.load(); }
    int protocol() const { return protocol_.load(); }
private:
    friend class pubsub_broker;
    pubsub_subscriber(uint64_t id, std::shared_ptr<connection_output> output,
        int protocol, std::shared_ptr<acl_identity> identity,
        std::function<void(bool)> disconnect,
        std::function<void(bool)> mode_changed);
    const uint64_t id_;
    std::shared_ptr<connection_output> output_;
    std::shared_ptr<acl_identity> identity_;
    std::function<void(bool)> disconnect_, mode_changed_;
    std::array<std::set<std::string>, 3> names_;
    std::atomic<int> protocol_;
    std::atomic<size_t> owned_{0};
    mutable std::mutex counts_mutex_;
    pubsub_counts counts_;
};
struct pubsub_reply {
    connection_output::ticket barrier = 0;
    std::string acknowledgements;
};
// Membership and enqueue order share one lock. No socket I/O occurs here.
// Callbacks must not throw, reenter the broker or wait for network I/O.
class pubsub_broker {
public:
    using subscriber = std::shared_ptr<pubsub_subscriber>;
    subscriber attach(uint64_t id, std::shared_ptr<connection_output> output,
        int protocol, std::shared_ptr<acl_identity> identity,
        std::function<void(bool)> disconnect,
        std::function<void(bool)> mode_changed = {});
    void retire(const subscriber& client);
    pubsub_reply change(const subscriber& client, pubsub_kind kind,
        const std::vector<const std::string*>& names, bool subscribe);
    connection_output::ticket synchronize(const subscriber& client,
        int protocol, std::shared_ptr<acl_identity> identity, bool reset);
    size_t publish(pubsub_kind kind, const std::string& channel,
        const std::string& message, uint64_t origin = 0);
    std::vector<std::string> channels(pubsub_kind kind,
        const std::string* pattern = nullptr) const;
    std::vector<size_t> subscribers(pubsub_kind kind,
        const std::vector<const std::string*>& names) const;
    pubsub_counts counts() const;
    void validate_permissions(
        const std::shared_ptr<acl_identity>& identity = nullptr,
        uint64_t executing = 0);
private:
    using members = std::map<uint64_t, subscriber>;
    using routes = std::map<std::string, members>;
    mutable std::mutex mutex_;
    std::array<routes, 3> routes_;
    std::map<uint64_t, std::weak_ptr<pubsub_subscriber>> clients_;
    bool contains(const subscriber& client) const;
    static size_t count(const pubsub_subscriber& client, size_t kind);
    static void refresh(pubsub_subscriber& client);
    void erase(pubsub_subscriber& client, size_t kind,
        const std::string& name);
    void clear(pubsub_subscriber& client);
    static bool allowed(const pubsub_subscriber& client);
    size_t deliver(const members& clients, const std::string& channel,
        const std::string& message, const std::string* pattern,
        bool shard, uint64_t origin);
};
} // namespace pkv
