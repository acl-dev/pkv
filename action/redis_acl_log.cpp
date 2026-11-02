#include "stdafx.h"
#include "redis_server.h"
#include "redis_handler.h"
#include "redis_service.h"
#include "common/execution_gate.h"
#include "common/redis_double.h"
#include "common/redis_integer.h"

namespace pkv {
namespace {
void text(redis_object& reply, const char* key, const std::string& value) {
    reply.create_child().set_string(key);
    reply.create_child().set_string(value);
}
void number(redis_object& reply, const char* key, long long value) {
    reply.create_child().set_string(key);
    reply.create_child().set_number(value);
}
} // namespace
bool redis_server::acl_log(redis_object& reply) {
    auto& audit = handler_.audit();
    long long count = 10;
    if (obj_.size() == 3) {
        const auto& argument = *obj_.argument(2);
        if (!strcasecmp(argument.c_str(), "reset")) {
            reply.set_status("OK"); audit.clear(); return true;
        }
        if (!parse_redis_integer(argument, count)) {
            reply.set_error("ERR value is not an integer or out of range");
            return true;
        }
    }
    const auto entries = audit.snapshot(count > 0 ?
        static_cast<size_t>(count) : 0);
    const auto now = execution_time_ms();
    reply.set_type(REDIS_OBJ_ARRAY);
    for (const auto& entry : entries) {
        auto& row = reply.create_child(); row.set_type(REDIS_OBJ_ARRAY);
        row.response_as(redis_object::response_kind::map);
        number(row, "count", entry.count);
        text(row, "reason", entry.reason); text(row, "context", entry.context);
        text(row, "object", entry.object);
        text(row, "username", entry.username);
        row.create_child().set_string("age-seconds");
        row.create_child().set_string(format_redis_double(
            (static_cast<double>(now) - entry.updated) / 1000.0)).
            response_as(redis_object::response_kind::real);
        text(row, "client-info", entry.client_info);
        number(row, "entry-id", entry.id);
        number(row, "timestamp-created", entry.created);
        number(row, "timestamp-last-updated", entry.updated);
    }
    return true;
}
} // namespace pkv
