#include "stdafx.h"
#include "redis_server.h"
#include "redis_handler.h"
#include "redis_service.h"
#include "common/redis_integer.h"
#include "common/execution_gate.h"
#include <climits>
#include <cstring>

namespace pkv {
namespace {
bool option(const std::string& value, const char* name) {
    return value.size() == std::strlen(name) &&
        !strncasecmp(value.data(), name, value.size());
}
std::string safe_token(const std::string& value) {
    auto text = value.substr(0, value.find('\0'));
    for (auto& ch : text) if (ch == '\r' || ch == '\n') ch = ' ';
    return text;
}
struct kill_filter {
    bool legacy = false, skipme = true;
    int type = -1;
    long long id = 0, maxage = 0;
    uint64_t user_id = 0;
    const std::string* address = nullptr;
    const std::string* local = nullptr;
};
bool parse_filter(const redis_object& request, kill_filter& filter,
    redis_object& reply, const acl_policy& policy) {
    if (request.size() == 3) {
        filter.legacy = true; filter.skipme = false;
        filter.address = request.argument(2); return true;
    }
    for (size_t i = 2; i < request.size(); i += 2) {
        if (i + 1 == request.size()) {
            reply.set_error("ERR syntax error"); return false;
        }
        const auto& name = *request.argument(i);
        const auto& value = *request.argument(i + 1);
        if (option(name, "ID")) {
            if (!parse_redis_integer(value, filter.id) || filter.id < 1 ||
                filter.id > LONG_MAX) {
                reply.set_error("ERR client-id should be greater than 0");
                return false;
            }
        } else if (option(name, "MAXAGE")) {
            if (!parse_redis_integer(value, filter.maxage)) {
                reply.set_error("ERR maxage is not an integer or out of range");
                return false;
            }
            if (filter.maxage <= 0) {
                reply.set_error("ERR maxage should be greater than 0");
                return false;
            }
        } else if (option(name, "TYPE")) {
            filter.type = option(value, "normal") ? 0 :
                option(value, "pubsub") ? 1 : 2;
            if (filter.type == 2 && !option(value, "master") &&
                !option(value, "slave") && !option(value, "replica") &&
                !option(value, "pubsub")) {
                reply.set_error("ERR Unknown client type '" +
                    safe_token(value) + "'"); return false;
            }
        } else if (option(name, "USER")) {
            const auto identity = policy.find(value);
            if (!identity) {
                reply.set_error("ERR No such user '" + safe_token(value) +
                    "'"); return false;
            }
            filter.user_id = identity->id;
        } else if (option(name, "ADDR")) filter.address = &value;
        else if (option(name, "LADDR")) filter.local = &value;
        else if (option(name, "SKIPME") && option(value, "YES"))
            filter.skipme = true;
        else if (option(name, "SKIPME") && option(value, "NO"))
            filter.skipme = false;
        else { reply.set_error("ERR syntax error"); return false; }
    }
    return true;
}
bool matches(const kill_filter& filter, const client_connection& entry,
    long long self, long long now) {
    if (filter.user_id) {
        client_snapshot metadata;
        if (!entry.read_metadata(metadata) ||
            metadata.user_id != filter.user_id)
            return false;
    }
    if (filter.type == 2) return false;
    if (filter.type >= 0) {
        client_view view;
        if (!entry.read_view(view, execution_time_ms())) return false;
        const auto& run = view.runtime;
        const bool subscribed = run.sub || run.psub || run.ssub;
        if (subscribed != (filter.type == 1)) return false;
    }
    return (!filter.skipme || entry.id() != self) &&
        (!filter.id || filter.id == entry.id()) &&
        (!filter.address || *filter.address == entry.address()) &&
        (!filter.local || *filter.local == entry.local_address()) &&
        (!filter.maxage || entry.age(now) >= filter.maxage);
}
} // namespace
bool redis_server::client_kill(redis_object& reply) {
    kill_filter filter;
    if (!parse_filter(obj_, filter, reply, handler_.permissions())) return true;
    const auto& registry = handler_.get_service().clients();
    std::vector<client_registry::connection> entries;
    if (filter.id) {
        auto entry = registry.find(filter.id);
        if (entry) entries.push_back(std::move(entry));
    } else entries = registry.snapshot(false);
    const auto self = handler_.client().id();
    const auto now = filter.maxage ? execution_time_ms() / 1000 : 0;
    entries.erase(std::remove_if(entries.begin(), entries.end(),
        [&](const client_registry::connection& entry) {
            return !matches(filter, *entry, self, now);
        }), entries.end());
    // Prepare all response storage before the first irreversible shutdown.
    std::string counter(20, '0');
    const std::string ok("OK"), missing("ERR No such client");
    if (filter.legacy) reply.set_error(missing);
    else reply.set_string(counter);
    unsigned long long killed = 0;
    for (const auto& entry : entries)
        if (entry->request_stop(entry->id() == self)) ++killed;
    if (filter.legacy) {
        if (killed) reply.set_status(ok);
    } else {
        size_t position = counter.size();
        do {
            counter[--position] = static_cast<char>('0' + killed % 10);
            killed /= 10;
        } while (killed);
        counter.erase(0, position);
        reply.set_string(counter);
        reply.set_type(REDIS_OBJ_INTEGER);
    }
    return true;
}
} // namespace pkv
