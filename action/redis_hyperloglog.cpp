#include "stdafx.h"
#include "redis_hyperloglog.h"
#include "redis_handler.h"
#include "dao/paged_string.h"

namespace pkv {
bool redis_hyperloglog::exec(const char* command, redis_coder& result) {
    const bool add = !strcasecmp(command, "PFADD");
    const bool count = !strcasecmp(command, "PFCOUNT");
    if (!add && !count && strcasecmp(command, "PFMERGE")) return false;
    const bool single = add || (count && obj_.size() == 2);
    std::vector<std::string> keys;
    if (!single) {
        for (size_t i = 1; i < obj_.size(); ++i)
            keys.push_back(*obj_.argument(i));
    }
    if (check_cluster_mode(true) &&
        (single ? route_keys(command, result) : route_keys(keys, result)))
        return true;
    dao::paged_string data;
    auto& reply = result.create_object();
    auto& database = handler_.get_db();
    long long value = 0;
    bool ok;
    if (add) {
        std::vector<const std::string*> elements;
        for (size_t i = 2; i < obj_.size(); ++i)
            elements.push_back(obj_.argument(i));
        ok = data.pfadd(database, *obj_.argument(1), elements, value);
    } else if (single) {
        ok = data.pfcount_one(database, *obj_.argument(1), value);
    } else if (count) ok = data.pfcount(database, keys, value);
    else ok = data.pfmerge(database, keys);
    if (!ok) reply.set_error(data.error());
    else if (add || count) reply.set_number(value);
    else reply.set_status("OK");
    return true;
}
} // namespace pkv
