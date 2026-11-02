#include "stdafx.h"
#include "redis_scan.h"
#include "redis_handler.h"
#include "dao/json/json_scan.h"

namespace pkv {
bool redis_scan::keys(redis_coder& result) {
    dao::json_scan data;
    std::unordered_set<std::string> keys;
    auto& reply = result.create_object();
    if (!data.all_keys(handler_.get_db(), *obj_.argument(1), keys)) {
        reply.set_error(data.error()); return true;
    }
    reply.set_type(REDIS_OBJ_ARRAY);
    for (const auto& key : keys) reply.create_child().set_string(key);
    return true;
}
} // namespace pkv
