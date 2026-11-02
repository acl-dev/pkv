#include "stdafx.h"
#include "redis_scan.h"
#include "redis_handler.h"
#include "dao/json/json_scan.h"

namespace pkv {
bool redis_scan::random_key(redis_coder& result) {
    dao::json_scan data;
    std::string key;
    bool found = false;
    auto& reply = result.create_object();
    if (!data.random_key(handler_.get_db(), key, found))
        reply.set_error(data.error());
    else if (found) reply.set_string(key);
    else reply.set_null();
    return true;
}
} // namespace pkv
