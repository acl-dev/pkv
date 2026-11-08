//
// Created by shuxin zheng on 2023/8/18.
//

#pragma once
#include <unordered_map>
#include <cstddef>
#include <string>
#include <functional>
#include <vector>
#include "common/password_policy.h"
#include "common/acl_policy.h"
#include "common/acl_audit.h"
#include "common/configuration.h"
#include "common/server_statistics.h"
#include "common/process_info.h"
#include "common/client_registry.h"
#include "common/pubsub.h"
#include "redis_dispatch.h"

namespace pkv {

class redis_handler;
class redis_object;
class redis_coder;
class slave_watcher;
class move_journal;

enum class redis_command_kind {
    regular, acl, config, info, client, keys, randomkey, dbsize, exec,
    blocking, pubsub, move
};

struct redis_command_spec {
    redis_handler_t handler;
    const char* name;
    size_t min_args;
    size_t max_args; // zero means no fixed upper bound
    size_t statistic;
    redis_command_kind kind;
    bool writes;
    bool valid_arity(size_t count) const {
        return count >= min_args && (max_args == 0 || count <= max_args);
    }
};

class redis_service {
public:
    redis_service();
    ~redis_service() = default;

    void init();
    process_identity& identity() { return identity_; }
    server_statistics& statistics() { return statistics_; }
    pubsub_broker& pubsub() { return pubsub_; }
    client_registry& clients() { return clients_; }
    const client_registry& clients() const { return clients_; }
    void password(std::string value) {
        std::string frontend(value);
        const std::vector<std::string> rules{"reset", "on", "allkeys",
            "allchannels", "+@all", value.empty() ? "nopass" : ">" + value};
        std::vector<const std::string*> borrowed;
        for (const auto& rule : rules) borrowed.push_back(&rule);
        permissions_.set("default", borrowed);
        passwords_ = password_policy(std::move(value));
        config_password_.swap(frontend);
    }
    const std::string& config_password() const { return config_password_; }
    // Startup or exclusive server execution admission is required.
    std::string configure(config_change& change);
    const password_policy& passwords() const { return passwords_; }
    acl_audit& audit() { return audit_; }
    acl_policy& permissions() { return permissions_; }
    const acl_policy& permissions() const { return permissions_; }
    const std::string& acl_file() const { return acl_file_; }
    std::string acl_file(std::string path);
    void replica(bool value) { replica_ = value; }
    bool replica() const { return replica_; }
    bool replica_read_only() const { return replica_read_only_; }
    void replica_read_only(bool value) { replica_read_only_ = value; }
    void set_replication_watcher(slave_watcher* watcher) {
        replication_ = watcher;
    }
    slave_watcher* replication_watcher() const { return replication_; }
    // Bound during startup; the master domain outlives all handlers.
    void set_move_journal(move_journal* journal) { move_journal_ = journal; }
    move_journal* journal() const { return move_journal_; }

    const redis_command_spec* get_command(const char* cmd) const;
    size_t command_count() const { return commands_.size(); }
    std::vector<std::string> command_names() const;

private:
    client_registry clients_;
    pubsub_broker pubsub_;
    password_policy passwords_;
    acl_policy permissions_;
    acl_audit audit_;
    std::string acl_file_;
    bool replica_ = false, replica_read_only_ = true;
    slave_watcher* replication_ = nullptr;
    move_journal* move_journal_ = nullptr;
    std::unordered_map<std::string, redis_command_spec> commands_;
    std::string config_password_;
    server_statistics statistics_;
    process_identity identity_;
    void add(const char* name, redis_handler_t handler, size_t min, size_t max,
        redis_command_kind kind = redis_command_kind::regular);

    static bool pubsub_handler(redis_handler&, const char*,
        const redis_object&, redis_coder& result);
    static bool key_handler(redis_handler&, const char*,
        const redis_object&, redis_coder& result);
    static bool geo_handler(redis_handler&, const char*,
        const redis_object&, redis_coder& result);
    static bool hll_handler(redis_handler&, const char*,
        const redis_object&, redis_coder& result);
    static bool string_handler(redis_handler&, const char*,
        const redis_object&, redis_coder& result);
    static bool hash_handler(redis_handler&, const char*,
        const redis_object&, redis_coder& result);
    static bool zset_handler(redis_handler&, const char*,
        const redis_object&, redis_coder& result);
    static bool scan_handler(redis_handler&, const char*,
        const redis_object&, redis_coder& result);
    static bool set_handler(redis_handler&, const char*,
        const redis_object&, redis_coder& result);
    static bool list_handler(redis_handler&, const char*,
        const redis_object&, redis_coder& result);
    static bool server_handler(redis_handler&, const char*,
        const redis_object&, redis_coder& result);
    static bool cluster_handler(redis_handler&, const char*,
        const redis_object&, redis_coder& result);
};

} // namespace pkv
