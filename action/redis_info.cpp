#include "stdafx.h"
#include "redis_server.h"
#include "redis_handler.h"
#include "redis_service.h"
#include "common/info_format.h"
#include "common/execution_gate.h"
#include "db/database_pool.h"
#include <stdexcept>

namespace pkv {
std::vector<std::pair<size_t, shared_db>> redis_handler::info_databases() {
    return databases_->materialized();
}
namespace {
void clients(std::string& out, redis_service& service) {
    uint64_t connected = 0, watched = 0, input = 0, output = 0, memory = 0;
    uint64_t blocked = 0, pubsub = 0;
    const auto now = execution_time_ms();
    for (const auto& connection : service.clients().snapshot(false)) {
        client_view view;
        if (!connection->read_view(view, now)) continue;
        ++connected; watched += view.runtime.watches;
        blocked += view.runtime.blocked;
        pubsub += view.runtime.sub || view.runtime.psub || view.runtime.ssub;
        input += view.runtime.query_size;
        output += view.runtime.output_heap;
        memory += view.runtime.owned_memory;
    }
    info_header(out, "Clients");
    info_number(out, "connected_clients", connected);
    info_number(out, "blocked_clients", blocked);
    info_number(out, "pubsub_clients", pubsub);
    info_number(out, "pkv_watched_client_keys", watched);
    info_number(out, "pkv_client_query_bytes", input);
    info_number(out, "pkv_client_output_heap", output);
    info_number(out, "pkv_client_owned_memory", memory);
}
bool keyspace(std::string& out, redis_handler& handler) {
    info_header(out, "Keyspace");
    std::vector<std::pair<size_t, shared_db>> databases;
    try { databases = handler.info_databases(); }
    catch (const std::runtime_error&) { return false; }
    for (const auto& database : databases) {
        if (var_cfg_cluster_mode && database.first) continue;
        uint64_t keys, expires, average;
        if (database.second->keyspace(keys, expires, average) !=
            db::count_status::ok) return false;
        if (!keys) continue;
        info_text(out, "db" + std::to_string(database.first), "keys=" +
            std::to_string(keys) + ",expires=" + std::to_string(expires) +
            ",avg_ttl=" + std::to_string(average));
    }
    return true;
}
} // namespace
bool redis_server::info(redis_coder& result) {
    auto& reply = result.create_object();
    auto& service = handler_.config_service();
    std::vector<const std::string*> arguments;
    for (size_t i = 1; i < obj_.size(); ++i)
        arguments.push_back(obj_.argument(i));
    const info_sections sections(arguments);
    std::string out;
    if (sections.has("server")) {
        const auto own = service.clients().find(handler_.client().id());
        info_server(out, service.identity(), var_cfg_cluster_mode,
            own ? own->local_address() : "");
    }
    if (sections.has("clients")) clients(out, service);
    process_sample sample;
    if (sections.has("memory") || sections.has("cpu"))
        sample = sample_process();
    if (sections.has("memory")) info_memory(out, sample);
    if (sections.has("persistence")) {
        info_header(out, "Persistence");
        info_number(out, "loading", 0);
        info_number(out, "aof_enabled", 0);
        info_number(out, "rdb_bgsave_in_progress", 0);
        info_text(out, "pkv_storage_engine", handler_.get_db()->get_dbtype());
    }
    server_counters snapshot;
    if (sections.has("stats") || sections.has("commandstats") ||
        sections.has("errorstats")) snapshot = service.statistics().snapshot();
    if (sections.has("stats")) {
        info_stats(out, snapshot, service.audit().dropped());
        const auto counts = service.pubsub().counts();
        info_number(out, "pubsub_channels", counts.channel);
        info_number(out, "pubsub_patterns", counts.pattern);
        info_number(out, "pubsubshard_channels", counts.shard);
    }
    if (sections.has("replication")) {
        info_header(out, "Replication");
        info_text(out, "role", service.replica() ? "slave" : "master");
        info_text(out, "pkv_replication_protocol", "private-rpc");
    }
    if (sections.has("cpu")) info_cpu(out, sample);
    if (sections.has("modules") || sections.has("module_list"))
        info_header(out, "Modules");
    if (sections.has("commandstats"))
        info_commands(out, snapshot, service.statistics().names());
    if (sections.has("errorstats")) info_errors(out, snapshot);
    if (sections.has("cluster")) {
        info_header(out, "Cluster");
        info_number(out, "cluster_enabled", var_cfg_cluster_mode ? 1 : 0);
    }
    if (sections.has("keyspace") && !keyspace(out, handler_)) {
        reply.set_error("ERR keyspace statistics unavailable"); return true;
    }
    reply.set_string(out).response_as(redis_object::response_kind::verbatim);
    return true;
}
} // namespace pkv
