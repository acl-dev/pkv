#include "stdafx.h"
#include "redis_scan.h"
#include "redis_handler.h"
#include "dao/json/json_scan.h"
#include "common/redis_integer.h"

namespace pkv {
namespace {
bool option_is(const std::string& option, const char* name) {
    const auto size = std::strlen(name);
    return option.size() == size && !strncasecmp(option.data(), name, size);
}
void reply_page(redis_coder& result, const scan_page& page) {
    auto& reply = result.create_object();
    reply.set_type(REDIS_OBJ_ARRAY);
    reply.create_child().set_string(std::to_string(page.cursor));
    auto& values = reply.create_child();
    values.set_type(REDIS_OBJ_ARRAY);
    for (const auto& value : page.values) values.create_child().set_string(value);
}
}
bool redis_scan::exec(const char* cmd, redis_coder& result) {
    if (!strcasecmp(cmd, "KEYS")) return keys(result);
    if (!strcasecmp(cmd, "RANDOMKEY")) return random_key(result);
    const bool database = !strcasecmp(cmd, "SCAN"), hash = !strcasecmp(cmd, "HSCAN");
    const size_t cursor_arg = database ? 1 : 2;
    scan_options options;
    if (!parse_scan_cursor(*obj_.argument(cursor_arg), options.cursor)) {
        result.create_object().set_error("ERR invalid cursor"); return true;
    }
    if (!database && check_cluster_mode(true) && route_keys(cmd, result)) return true;
    dao::json_scan data;
    // Redis resolves collection absence/type after cursor validation but before
    // options. Missing collections return an empty scan even with bad options.
    if (!database && !data.open_collection(handler_.get_db(), *obj_.argument(1), hash ? value_kind::hash : !strcasecmp(cmd, "ZSCAN") ? value_kind::zset : value_kind::set)) {
        if (data.status() == dao::dao_base::read_status::missing) reply_page(result, scan_page());
        else result.create_object().set_error(data.error());
        return true;
    }
    for (size_t i = cursor_arg + 1; i < obj_.size(); ++i) {
        const auto& option = *obj_.argument(i);
        if (option_is(option, "NOVALUES")) {
            if (!hash) { result.create_object().set_error("ERR NOVALUES option can only be used in HSCAN"); return true; }
            options.no_values = true;
        } else if (i + 1 < obj_.size() && option_is(option, "COUNT")) {
            long long count;
            if (!parse_redis_integer(*obj_.argument(++i), count)) {
                result.create_object().set_error("ERR value is not an integer or out of range"); return true;
            }
            if (count < 1) { result.create_object().set_error("ERR syntax error"); return true; }
            options.count = static_cast<size_t>(count);
        } else if (i + 1 < obj_.size() && option_is(option, "MATCH")) {
            options.has_match = true;
            options.pattern = *obj_.argument(++i);
        } else if (database && i + 1 < obj_.size() && option_is(option, "TYPE")) {
            options.has_type = true;
            options.type = *obj_.argument(++i);
            for (char& ch : options.type) if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
        } else { result.create_object().set_error("ERR syntax error"); return true; }
    }
    scan_page page;
    if (!(database ? data.keys(handler_.get_db(), options, page) : data.collection(options, page))) {
        result.create_object().set_error(data.error()); return true;
    }
    reply_page(result, page);
    return true;
}
}
