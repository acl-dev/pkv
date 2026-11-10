#include "stdafx.h"
#include "redis_scan.h"
#include "redis_handler.h"
#include "dao/json/json_scan.h"
#include <limits>

namespace pkv {
bool redis_scan::keys(redis_coder& result) {
    dao::json_scan data;
    auto& reply = result.create_object();
    // Fill in the array length once matching and expiry checks are complete.
    constexpr size_t header_space = std::numeric_limits<size_t>::digits10 + 4;
    std::string frame(header_space, '\0');
    size_t count = 0;
    if (!data.visit_all_keys(handler_.get_db(), *obj_.argument(1),
        [&](const std::string& key) {
            frame += '$';
            frame += std::to_string(key.size());
            frame += "\r\n";
            frame += key;
            frame += "\r\n";
            ++count;
        })) {
        reply.set_error(data.error()); return true;
    }
    frame.replace(0, header_space, "*" + std::to_string(count) + "\r\n");
    reply.set_raw(std::move(frame));
    return true;
}
} // namespace pkv
