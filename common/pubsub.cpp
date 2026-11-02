#include "pubsub.h"
#include "acl_policy.h"
#include "client_memory.h"
#include "redis_glob.h"
#include <stdexcept>
namespace pkv {
namespace {
size_t index(pubsub_kind kind) {
    const auto value = static_cast<size_t>(kind);
    if (value >= 3) throw std::invalid_argument("invalid subscription kind");
    return value;
}
size_t name_memory(const std::string& name) {
    return sizeof(name) + 4 * sizeof(void*) + string_heap_bytes(name);
}
} // namespace
pubsub_subscriber::pubsub_subscriber(uint64_t id,
    std::shared_ptr<connection_output> output, int protocol,
    std::shared_ptr<acl_identity> identity,
    std::function<void(bool)> disconnect,
    std::function<void(bool)> mode_changed)
    : id_(id), output_(std::move(output)), identity_(std::move(identity)),
    disconnect_(std::move(disconnect)),
    mode_changed_(std::move(mode_changed)), protocol_(protocol) {}
pubsub_counts pubsub_subscriber::counts() const {
    std::lock_guard<std::mutex> guard(counts_mutex_); return counts_;
}
pubsub_broker::subscriber pubsub_broker::attach(uint64_t id,
    std::shared_ptr<connection_output> output, int protocol,
    std::shared_ptr<acl_identity> identity,
    std::function<void(bool)> disconnect,
    std::function<void(bool)> mode_changed) {
    if (!id || !output || (protocol != 2 && protocol != 3) || !disconnect)
        throw std::invalid_argument("invalid subscription client");
    subscriber client(new pubsub_subscriber(id, std::move(output), protocol,
        std::move(identity), std::move(disconnect), std::move(mode_changed)));
    std::lock_guard<std::mutex> guard(mutex_);
    const auto found = clients_.find(id);
    if (found != clients_.end() && !found->second.expired())
        throw std::invalid_argument("duplicate subscription client");
    clients_[id] = client;
    return client;
}
bool pubsub_broker::contains(const subscriber& client) const {
    if (!client) return false;
    const auto found = clients_.find(client->id_);
    return found != clients_.end() && found->second.lock() == client;
}
size_t pubsub_broker::count(const pubsub_subscriber& client, size_t kind) {
    return kind == 2 ? client.names_[2].size() :
        client.names_[0].size() + client.names_[1].size();
}
void pubsub_broker::refresh(pubsub_subscriber& client) {
    pubsub_counts counts;
    counts.channel = client.names_[0].size();
    counts.pattern = client.names_[1].size();
    counts.shard = client.names_[2].size();
    {
        std::lock_guard<std::mutex> guard(client.counts_mutex_);
        client.counts_ = counts;
    }
    // Do not hold the counts lock while a quota callback can stop a client:
    // registry inspectors acquire that lock while holding the client lock.
    if (client.mode_changed_) client.mode_changed_(counts.total() != 0);
}
void pubsub_broker::erase(pubsub_subscriber& client, size_t kind,
    const std::string& name) {
    auto& table = routes_[kind];
    const auto route = table.find(name);
    if (route != table.end()) {
        route->second.erase(client.id_);
        if (route->second.empty()) table.erase(route);
    }
    auto& names = client.names_[kind];
    const auto found = names.find(name);
    if (found != names.end()) {
        client.owned_.fetch_sub(name_memory(*found)); names.erase(found);
    }
}
void pubsub_broker::clear(pubsub_subscriber& client) {
    for (size_t kind = 0; kind < 3; ++kind)
        while (!client.names_[kind].empty())
            erase(client, kind, *client.names_[kind].begin());
    refresh(client);
}
void pubsub_broker::retire(const subscriber& client) {
    std::lock_guard<std::mutex> guard(mutex_);
    if (!contains(client)) return;
    clear(*client); clients_.erase(client->id_);
}
pubsub_reply pubsub_broker::change(const subscriber& client,
    pubsub_kind kind, const std::vector<const std::string*>& names,
    bool subscribe) {
    const auto which = index(kind);
    for (const auto* name : names)
        if (!name) throw std::invalid_argument("null subscription channel");
    if (subscribe && names.empty())
        throw std::invalid_argument("empty subscription request");
    std::lock_guard<std::mutex> guard(mutex_);
    pubsub_reply reply;
    if (!contains(client)) return reply;
    reply.barrier = client->output_->reserve_reply();
    if (!reply.barrier || !client->output_->capture_external()) return reply;
    auto& selected = client->names_[which];
    const auto protocol = client->protocol_.load();
    if (subscribe) {
        std::vector<const std::string*> added;
        added.reserve(names.size());
        try {
            for (const auto* name : names) {
                if (selected.find(*name) == selected.end()) {
                    const auto inserted = selected.insert(
                        copy_client_string(*name));
                    client->owned_.fetch_add(name_memory(*inserted.first));
                    // Track before route allocation, so every failing insert
                    // releases both indexes, including empty route entries.
                    added.push_back(name);
                    auto& table = routes_[which];
                    auto route = table.lower_bound(*name);
                    if (route == table.end() || route->first != *name)
                        route = table.emplace_hint(route,
                            copy_client_string(*name), members());
                    route->second.emplace(client->id_, client);
                }
                reply.acknowledgements += pubsub_ack(kind, true, name,
                    count(*client, which), protocol);
            }
        } catch (...) {
            for (const auto* name : added) erase(*client, which, *name);
            throw;
        }
    } else {
        std::set<std::string> removed;
        if (names.empty()) {
            auto remaining = count(*client, which);
            for (const auto& name : selected)
                reply.acknowledgements += pubsub_ack(kind, false, &name,
                    --remaining, protocol);
            if (selected.empty()) reply.acknowledgements = pubsub_ack(kind,
                false, nullptr, remaining, protocol);
            while (!selected.empty()) erase(*client, which, *selected.begin());
        } else {
            for (const auto* name : names) {
                if (selected.count(*name) && !removed.count(*name))
                    removed.insert(copy_client_string(*name));
                reply.acknowledgements += pubsub_ack(kind, false, name,
                    count(*client, which) - removed.size(), protocol);
            }
            for (const auto& name : removed) erase(*client, which, name);
        }
    }
    refresh(*client); return reply;
}
connection_output::ticket pubsub_broker::synchronize(const subscriber& client,
    int protocol, std::shared_ptr<acl_identity> identity, bool reset) {
    if (protocol != 2 && protocol != 3)
        throw std::invalid_argument("invalid subscription protocol");
    std::lock_guard<std::mutex> guard(mutex_);
    if (!contains(client)) return 0;
    const auto reply = client->output_->reserve_reply();
    if (!reply || !client->output_->capture_external()) return 0;
    if (reset) clear(*client);
    client->protocol_ = protocol; client->identity_ = std::move(identity);
    return reply;
}
size_t pubsub_broker::deliver(const members& clients,
    const std::string& channel, const std::string& message,
    const std::string* pattern, bool shard, uint64_t origin) {
    connection_output::frame frames[2];
    for (const auto& entry : clients) {
        const auto& client = *entry.second;
        if (client.output_->closed()) continue;
        const auto protocol = client.protocol_.load();
        auto& data = frames[protocol - 2];
        if (!data) data = std::make_shared<const std::string>(pubsub_message(
            channel, message, pattern, shard, protocol));
        client.output_->append(data, origin == client.id_);
    }
    return clients.size();
}
size_t pubsub_broker::publish(pubsub_kind kind, const std::string& channel,
    const std::string& message, uint64_t origin) {
    const auto which = index(kind);
    if (which == 1) throw std::invalid_argument("cannot publish to a pattern");
    std::lock_guard<std::mutex> guard(mutex_);
    size_t delivered = 0;
    const auto found = routes_[which].find(channel);
    if (found != routes_[which].end()) delivered += deliver(found->second,
        channel, message, nullptr, which == 2, origin);
    if (which == 0) for (const auto& pattern : routes_[1])
        if (redis_glob_match(pattern.first, channel))
            delivered += deliver(pattern.second, channel, message,
                &pattern.first, false, origin);
    return delivered;
}
std::vector<std::string> pubsub_broker::channels(pubsub_kind kind,
    const std::string* pattern) const {
    const auto which = index(kind);
    std::lock_guard<std::mutex> guard(mutex_);
    std::vector<std::string> result;
    for (const auto& route : routes_[which])
        if (!pattern || redis_glob_match(*pattern, route.first))
            result.push_back(copy_client_string(route.first));
    return result;
}
std::vector<size_t> pubsub_broker::subscribers(pubsub_kind kind,
    const std::vector<const std::string*>& names) const {
    const auto which = index(kind);
    std::lock_guard<std::mutex> guard(mutex_);
    std::vector<size_t> result; result.reserve(names.size());
    for (const auto* name : names) {
        if (!name) throw std::invalid_argument("null subscription channel");
        const auto found = routes_[which].find(*name);
        result.push_back(found == routes_[which].end() ?
            0 : found->second.size());
    }
    return result;
}
pubsub_counts pubsub_broker::counts() const {
    std::lock_guard<std::mutex> guard(mutex_);
    pubsub_counts result;
    result.channel = routes_[0].size(); result.pattern = routes_[1].size();
    result.shard = routes_[2].size(); return result;
}
bool pubsub_broker::allowed(const pubsub_subscriber& client) {
    if (!client.identity_) return true;
    if (client.identity_->revoked()) return false;
    const auto user = client.identity_->snapshot();
    for (const auto& selector : user->selectors)
        if (selector.all_channels) return true;
    for (size_t which = 0; which < 3; ++which)
        for (const auto& name : client.names_[which]) {
            bool permitted = false;
            for (const auto& selector : user->selectors)
                if (acl_channel_allowed(selector, name, which == 1)) {
                    permitted = true; break;
                }
            if (!permitted) return false;
        }
    return true;
}
void pubsub_broker::validate_permissions(
    const std::shared_ptr<acl_identity>& identity, uint64_t executing) {
    std::lock_guard<std::mutex> guard(mutex_);
    for (auto position = clients_.begin(); position != clients_.end();) {
        const auto client = position->second.lock();
        if (!client) { position = clients_.erase(position); continue; }
        if ((!identity || identity == client->identity_) && !allowed(*client))
            client->disconnect_(client->id_ == executing);
        ++position;
    }
}
} // namespace pkv
