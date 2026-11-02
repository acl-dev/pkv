//
// Created by shuxin zheng on 2023/8/1.
//

#include "stdafx.h"
#include "common/redis_slot.h"
#include "common/command_keys.h"
#include "coder/redis_coder.h"
#include "coder/redis_object.h"
#include "redis_command.h"

namespace pkv {

redis_command::redis_command(redis_handler& handler, const redis_object& obj)
: handler_(handler), obj_(obj)
{}

bool redis_command::check_cluster_mode(bool on) const {
    return var_cfg_cluster_mode != 0 && on;
}

bool redis_command::route_keys(const char* cmd, redis_coder& result) {
    if (!var_cfg_cluster_mode) return false;
    if (!strcasecmp(cmd, "MSETEX")) {
        std::vector<const std::string*> keys;
        for (size_t i = 0; i < obj_.size(); ++i)
            keys.push_back(obj_.argument(i));
        if (!command_keys(keys) || keys.empty()) return false;
        const auto slot = redis_hash_slot(*keys.front());
        for (const auto* key : keys) if (redis_hash_slot(*key) != slot) {
            result.create_object().set_error(
                "CROSSSLOT Keys in request don't hash to the same slot");
            return true;
        }
        return route_slot(*keys.front(), slot, result);
    }
    if (!strcasecmp(cmd, "BITOP")) {
        std::vector<std::string> keys;
        for (size_t i = 2; i < obj_.size(); ++i)
            keys.push_back(*obj_.argument(i));
        return route_keys(keys, result);
    }
    const bool pairs = !strcasecmp(cmd, "MSET") || !strcasecmp(cmd, "MSETNX");
    const bool two = !strcasecmp(cmd, "RENAME") ||
        !strcasecmp(cmd, "RENAMENX") || !strcasecmp(cmd, "LCS");
    const bool all = pairs || two || !strcasecmp(cmd, "MGET") || !strcasecmp(cmd, "DEL") || !strcasecmp(cmd, "EXISTS");
    // Validate pair arity before checking slots, so an incomplete request cannot
    // be interpreted as a second key or redirected before its syntax error.
    if (pairs && obj_.size() % 2 != 1) return false;
    const auto* first = obj_.argument(1);
    if (!first) return false;
    const size_t slot = redis_hash_slot(*first);
    const size_t end = two ? std::min(obj_.size(), static_cast<size_t>(3)) : obj_.size();
    if (all) for (size_t i = pairs ? 3 : 2; i < end; i += pairs ? 2 : 1) {
        if (redis_hash_slot(*obj_.argument(i)) != slot) {
            result.create_object().set_error("CROSSSLOT Keys in request don't hash to the same slot");
            return true;
        }
    }
    return route_slot(*first, slot, result);
}

bool redis_command::route_keys(const std::vector<std::string>& keys, redis_coder& result) {
    if (!var_cfg_cluster_mode || keys.empty()) return false;
    const auto slot = redis_hash_slot(keys.front());
    for (const auto& key : keys) if (redis_hash_slot(key) != slot) {
        result.create_object().set_error("CROSSSLOT Keys in request don't hash to the same slot");
        return true;
    }
    return route_slot(keys.front(), slot, result);
}

bool redis_command::route_slot(const std::string& key, size_t slot, redis_coder& result) {
    size_t found_slot;
    auto node = cluster_manager::get_instance().get_node(key, found_slot);
    if (!node) {
        result.create_object().set_error("CLUSTERDOWN Hash slot not served");
        return true;
    }
    const auto& address = node->get_addr();
    if (address != var_cfg_service_addr) return redirect(address, slot, result);
    return false;
}

bool redis_command::redirect(const std::string &addr, size_t slot,
      redis_coder& result) {
    std::string buf("MOVED");
    buf += " ";
    buf += std::to_string(slot);
    buf += " ";
    buf += addr;
    result.create_object().set_error(buf);
    return true;
}

} // namespace pkv
