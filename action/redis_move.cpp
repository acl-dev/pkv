#include "stdafx.h"
#include "redis_key.h"
#include "redis_handler.h"
#include "redis_service.h"
#include "coder/redis_object.h"
#include "common/redis_integer.h"
#include "dao/move_key.h"
#include "slave/replica_scope.h"
#include <climits>

namespace pkv {
bool redis_key::move(redis_coder& result) {
    if (check_cluster_mode(true) && route_keys("MOVE", result)) return true;
    auto& reply = result.create_object();
    const auto error = [&](const char* message) {
        reply.set_error(message); return true;
    };
    if (var_cfg_cluster_mode)
        return error("ERR MOVE is not allowed in cluster mode");
    long long destination;
    if (!parse_redis_integer(*obj_.argument(2), destination))
        return error("ERR value is not an integer or out of range");
    if (destination < INT_MIN || destination > INT_MAX)
        return error("ERR value is out of range, value must between "
            "-2147483648 and 2147483647");
    if (destination < 0 || static_cast<unsigned long long>(destination) >=
        handler_.database_count()) return error("ERR DB index is out of range");
    const auto from = handler_.selected_database();
    const auto to = static_cast<size_t>(destination);
    if (from == to)
        return error("ERR source and destination objects are the same");
    replica_scope replication(handler_.get_service().replication_watcher());
    dao::move_key data;
    bool moved = false;
    if (!data.run(handler_.databases(), handler_.get_service().journal(),
        from, to, *obj_.argument(1), moved)) return error(data.error());
    reply.set_number(moved ? 1 : 0); return true;
}
} // namespace pkv
