#include "stdafx.h"
#include "redis_server.h"
#include "redis_handler.h"
#include "redis_service.h"
#include "common/command_catalog.h"

namespace pkv {
std::string redis_service::configure(config_change& change) {
    if (change.password && change.value != config_password_) {
        const std::vector<std::string> rules{"resetpass",
            change.value.empty() ? "nopass" : ">" + change.value};
        std::vector<const std::string*> borrowed;
        for (const auto& rule : rules) borrowed.push_back(&rule);
        const auto error = permissions_.set("default", borrowed);
        if (!error.empty()) return error;
        config_password_.swap(change.value);
    }
    if (change.audit) audit_.defer_limit(change.limit);
    if (change.replica_policy) replica_read_only_ = change.replica_read_only;
    return {};
}
bool redis_handler::config_command_valid(const redis_object& object) {
    const auto child = command_lower(*object.argument(1));
    const auto* entry = command_catalog("config|" + child);
    if (entry && ((entry->arity > 0 && object.size() != size_t(entry->arity)) ||
        (entry->arity < 0 && object.size() < size_t(-entry->arity)))) {
        transaction_.fail();
        builder_.create_object().set_error("ERR wrong number of arguments "
            "for '" + std::string(entry->name) + "' command"); return false;
    }
    if (!entry) {
        transaction_.fail();
        builder_.create_object().set_error("ERR unknown subcommand '" +
            acl_safe_token(*object.argument(1)) +
            "'. Try CONFIG HELP."); return false;
    }
    return true;
}
bool redis_server::config(redis_coder& result) {
    auto& reply = result.create_object();
    const auto child = command_lower(*obj_.argument(1));
    if (child == "get") return config_get(reply);
    if (child == "set") return config_set(reply);
    if (child == "resetstat") {
        reply.set_status("OK");
        handler_.config_service().statistics().reset(); return true;
    }
    reply.set_type(REDIS_OBJ_ARRAY);
    for (const auto* line : {"GET <pattern> [<pattern> ...]",
        "SET <parameter> <value> [<parameter> <value> ...]",
        "RESETSTAT", "HELP"})
        reply.create_child().set_string(line);
    return true;
}
bool redis_server::config_get(redis_object& reply) {
    auto& service = handler_.config_service();
    reply.set_type(REDIS_OBJ_ARRAY);
    reply.response_as(redis_object::response_kind::map);
    for (const auto& name : config_names()) {
        bool found = false;
        std::string label(name);
        for (size_t i = 2; i < obj_.size() && !found; ++i) {
            const auto& pattern = *obj_.argument(i);
            found = config_matches(pattern, name);
            if (found && !config_pattern(pattern)) label = pattern;
        }
        if (!found) continue;
        std::string value;
        if (name == "requirepass") value = service.config_password();
        else if (name == "acllog-max-len")
            value = std::to_string(service.audit().limit());
        else if (name == "aclfile") value = service.acl_file();
        else if (name == "databases")
            value = std::to_string(var_cfg_cluster_mode ? 1 :
                handler_.database_count());
        else if (name == "cluster-enabled")
            value = var_cfg_cluster_mode ? "yes" : "no";
        else if (name == "appendonly") value = "no";
        else if (name == "replica-read-only" || name == "slave-read-only")
            value = service.replica_read_only() ? "yes" : "no";
        reply.create_child().set_string(label);
        reply.create_child().set_string(value);
    }
    return true;
}
bool redis_server::config_set(redis_object& reply) {
    std::vector<const std::string*> pairs;
    for (size_t i = 2; i < obj_.size(); ++i) pairs.push_back(obj_.argument(i));
    config_change change;
    auto error = config_prepare(pairs, change);
    if (!error.empty()) { reply.set_error(error); return true; }
    reply.set_status("OK");
    error = handler_.config_service().configure(change);
    if (!error.empty()) reply.set_error(error);
    return true;
}
} // namespace pkv
