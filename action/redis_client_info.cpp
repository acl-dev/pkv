#include "stdafx.h"
#include "redis_server.h"
#include "redis_handler.h"
#include "redis_service.h"
#include "common/execution_gate.h"
#include "common/client_description.h"
#include "common/redis_integer.h"
#include <algorithm>
#include <cstring>

namespace pkv {
namespace {
bool option(const std::string& value, const char* name) {
    return value.size() == std::strlen(name) &&
        !strncasecmp(value.data(), name, value.size());
}
std::string diagnostic(const std::string& value) {
    auto out = value.substr(0, value.find('\0'));
    for (auto& ch : out) if (ch == '\r' || ch == '\n') ch = ' ';
    return out;
}
} // namespace
bool redis_server::client_info(redis_object& reply, bool list) {
    const auto& registry = handler_.get_service().clients();
    std::vector<client_registry::connection> entries;
    int filter = -1;
    if (!list) {
        auto entry = registry.find(handler_.client().id());
        if (entry) entries.push_back(std::move(entry));
    } else if (obj_.size() == 2) entries = registry.snapshot(false);
    else if (obj_.size() == 4 && option(*obj_.argument(2), "TYPE")) {
        const auto& type = *obj_.argument(3);
        if (option(type, "normal") || option(type, "pubsub")) {
            entries = registry.snapshot(false);
            filter = option(type, "pubsub") ? 1 : 0;
        } else if (!option(type, "master") && !option(type, "slave") &&
            !option(type, "replica") && !option(type, "pubsub")) {
            reply.set_error("ERR Unknown client type '" + diagnostic(type) +
                "'"); return true;
        }
    } else if (obj_.size() > 3 && option(*obj_.argument(2), "ID")) {
        std::vector<long long> ids;
        ids.reserve(obj_.size() - 3);
        for (size_t i = 3; i < obj_.size(); ++i) {
            long long id;
            if (!parse_redis_integer(*obj_.argument(i), id)) {
                reply.set_error("ERR Invalid client ID"); return true;
            }
            ids.push_back(id);
        }
        entries.reserve(ids.size());
        for (auto id : ids) {
            auto entry = registry.find(id);
            if (entry) entries.push_back(std::move(entry));
        }
    } else { reply.set_error("ERR syntax error"); return true; }
    const auto now = execution_time_ms();
    std::string text;
    for (const auto& entry : entries)
        append_client_description(text, entry, now, filter);
    reply.set_string(text).response_as(redis_object::response_kind::verbatim);
    return true;
}
} // namespace pkv
