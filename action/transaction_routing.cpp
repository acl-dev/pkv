#include "stdafx.h"
#include "redis_handler.h"
#include "coder/redis_object.h"
#include "cluster/cluster_manager.h"
#include "common/command_keys.h"
#include "common/redis_slot.h"

namespace pkv {
namespace {
void routing_arguments(std::vector<const std::string*>& arguments) {
    if (arguments.empty()) return;
    const auto& name = *arguments.front();
    if (!strcasecmp(name.c_str(), "SSUBSCRIBE")) {
        arguments.erase(arguments.begin()); return;
    }
    if (!strcasecmp(name.c_str(), "SPUBLISH")) {
        const auto* channel = arguments.size() > 1 ? arguments[1] : nullptr;
        arguments.clear();
        if (channel) arguments.push_back(channel);
        return;
    }
    command_keys(arguments);
}
} // namespace

bool redis_handler::route_transaction_keys(
    const std::vector<const std::string*>& keys, const std::string*& first) {
    if (!var_cfg_cluster_mode) return false;
    for (const auto* key : keys) {
        if (!first) first = key;
        else if (redis_hash_slot(*first) != redis_hash_slot(*key)) {
            builder_.create_object().set_error(
                "CROSSSLOT Keys in request don't hash to the same slot");
            return true;
        }
    }
    return false;
}

bool redis_handler::route_transaction_owner(const std::string* first) {
    if (!var_cfg_cluster_mode || !first) return false;
    size_t slot;
    auto node = cluster_manager::get_instance().get_node(*first, slot);
    if (!node) {
        builder_.create_object().set_error("CLUSTERDOWN Hash slot not served");
        return true;
    }
    if (node->get_addr() != var_cfg_service_addr) {
        builder_.create_object().set_error("MOVED " + std::to_string(slot) +
            " " + node->get_addr());
        return true;
    }
    return false;
}

bool redis_handler::route_queued_command(const redis_object& object) {
    if (!var_cfg_cluster_mode) return false;
    std::vector<const std::string*> keys;
    keys.reserve(object.size());
    for (size_t i = 0; i < object.size(); ++i)
        keys.push_back(object.argument(i));
    routing_arguments(keys);
    const std::string* first = nullptr;
    return route_transaction_keys(keys, first) ||
        route_transaction_owner(first);
}

bool redis_handler::route_transaction() {
    if (!var_cfg_cluster_mode) return false;
    const std::string* first = nullptr;
    for (const auto& command : transaction_.commands()) {
        std::vector<const std::string*> keys;
        keys.reserve(command.size());
        for (const auto& argument : command) keys.push_back(&argument);
        routing_arguments(keys);
        if (route_transaction_keys(keys, first)) return true;
    }
    return route_transaction_owner(first);
}

} // namespace pkv
