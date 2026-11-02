#include "stdafx.h"
#include "redis_pubsub.h"
#include "redis_handler.h"
#include "redis_service.h"
#include "common/command_catalog.h"
#include "common/redis_slot.h"
namespace pkv {
bool redis_pubsub::route(bool all, redis_coder& result) {
    if (!var_cfg_cluster_mode || obj_.size() < 2) return false;
    const auto& first = *obj_.argument(1);
    const auto slot = redis_hash_slot(first);
    if (all) for (size_t i = 2; i < obj_.size(); ++i)
        if (redis_hash_slot(*obj_.argument(i)) != slot) {
            result.create_object().set_error(
                "CROSSSLOT Keys in request don't hash to the same slot");
            return true;
        }
    return route_slot(first, slot, result);
}
bool redis_pubsub::change(pubsub_kind kind, bool subscribe,
    redis_coder& result) {
    if (kind == pubsub_kind::shard && subscribe && route(true, result))
        return true;
    // Reserve the response before membership changes. Moving the completed
    // frame into it cannot fail after the broker commits the new membership.
    auto& response = result.create_object();
    try {
        std::vector<const std::string*> names;
        names.reserve(obj_.size() - 1);
        for (size_t i = 1; i < obj_.size(); ++i)
            names.push_back(obj_.argument(i));
        const auto client = handler_.ensure_pubsub();
        auto reply = handler_.service_.pubsub().change(client, kind, names,
            subscribe);
        handler_.output_reply_ = reply.barrier;
        if (!reply.barrier) { handler_.disconnected(); return false; }
        response.set_raw(std::move(reply.acknowledgements));
    } catch (const std::bad_alloc&) {
        response.set_error("OOM pubsub allocation failed");
    }
    return true;
}
bool redis_pubsub::publish(bool shard, redis_coder& result) {
    if (shard && route(false, result)) return true;
    const auto count = handler_.service_.pubsub().publish(shard ?
        pubsub_kind::shard : pubsub_kind::channel, *obj_.argument(1),
        *obj_.argument(2), static_cast<uint64_t>(handler_.client().id()));
    result.create_object().set_number(static_cast<long long>(count));
    return true;
}
bool redis_pubsub::query(redis_coder& result) {
    const auto name = command_lower(*obj_.argument(1));
    auto& reply = result.create_object();
    if ((name == "channels" || name == "shardchannels") && obj_.size() > 3) {
        reply.set_error("ERR unknown subcommand or wrong number of "
            "arguments for '" + pubsub_error_token(*obj_.argument(1)) +
            "'. Try PUBSUB HELP."); return true;
    }
    auto& broker = handler_.service_.pubsub();
    const auto kind = name == "shardchannels" || name == "shardnumsub" ?
        pubsub_kind::shard : pubsub_kind::channel;
    if (name == "numpat") reply.set_number(broker.counts().pattern);
    else if (name == "channels" || name == "shardchannels") {
        reply.set_type(REDIS_OBJ_ARRAY);
        for (const auto& channel : broker.channels(kind,
            obj_.size() == 3 ? obj_.argument(2) : nullptr))
            reply.create_child().set_string(channel);
    } else if (name == "numsub" || name == "shardnumsub") {
        std::vector<const std::string*> names;
        for (size_t i = 2; i < obj_.size(); ++i)
            names.push_back(obj_.argument(i));
        const auto counts = broker.subscribers(kind, names);
        reply.set_type(REDIS_OBJ_ARRAY);
        for (size_t i = 0; i < names.size(); ++i) {
            reply.create_child().set_string(*names[i]);
            reply.create_child().set_number(counts[i]);
        }
    } else if (name == "help") {
        reply.set_type(REDIS_OBJ_ARRAY);
        for (const auto* text : {"CHANNELS [pattern]", "NUMPAT",
            "NUMSUB [channel ...]", "SHARDCHANNELS [pattern]",
            "SHARDNUMSUB [shardchannel ...]", "HELP"})
            reply.create_child().set_string(text);
    } else reply.set_error("ERR unknown subcommand '" +
        pubsub_error_token(*obj_.argument(1)) + "'. Try PUBSUB HELP.");
    return true;
}
bool redis_pubsub::exec(const char* command, redis_coder& result) {
    try {
        if (!strcasecmp(command, "SUBSCRIBE"))
            return change(pubsub_kind::channel, true, result);
        if (!strcasecmp(command, "UNSUBSCRIBE"))
            return change(pubsub_kind::channel, false, result);
        if (!strcasecmp(command, "PSUBSCRIBE"))
            return change(pubsub_kind::pattern, true, result);
        if (!strcasecmp(command, "PUNSUBSCRIBE"))
            return change(pubsub_kind::pattern, false, result);
        if (!strcasecmp(command, "SSUBSCRIBE"))
            return change(pubsub_kind::shard, true, result);
        if (!strcasecmp(command, "SUNSUBSCRIBE"))
            return change(pubsub_kind::shard, false, result);
        if (!strcasecmp(command, "PUBLISH")) return publish(false, result);
        if (!strcasecmp(command, "SPUBLISH")) return publish(true, result);
        return query(result);
    } catch (const std::bad_alloc&) {
        result.create_object().set_error("OOM pubsub allocation failed");
        return true;
    }
}
} // namespace pkv
