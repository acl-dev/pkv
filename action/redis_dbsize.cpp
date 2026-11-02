#include "stdafx.h"
#include "redis_server.h"
#include "redis_handler.h"
#include <climits>

namespace pkv {
bool redis_server::dbsize(redis_coder& result) {
    uint64_t count;
    const auto status = handler_.get_db()->key_count(count);
    auto& reply = result.create_object();
    if (status == db::count_status::unsupported)
        reply.set_error("ERR key counting is not supported by this backend");
    else if (status != db::count_status::ok || count > LLONG_MAX)
        reply.set_error("ERR storage operation failed");
    else reply.set_number(static_cast<long long>(count));
    return true;
}
} // namespace pkv
