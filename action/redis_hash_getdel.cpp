#include "stdafx.h"
#include "redis_hash.h"
#include "redis_handler.h"
#include "dao/hash.h"
#include "common/redis_integer.h"
#include <climits>
#include <cstring>

namespace pkv {
bool redis_hash::hgetdel(redis_coder& result) {
    std::string error;
    const auto parse = [&](std::vector<std::string>& fields) {
        const auto& token = *obj_.argument(2);
        if (token.size() != 6 || strncasecmp(token.data(), "FIELDS", 6))
            error = "ERR Mandatory argument FIELDS is missing or not at the "
                "right position";
        else {
            long long count;
            if (!parse_redis_integer(*obj_.argument(3), count) || count < 1 ||
                count > LONG_MAX)
                error = "ERR Number of fields must be a positive integer";
            else if (static_cast<unsigned long long>(count) !=
                obj_.size() - 4)
                error = "ERR The `numfields` parameter must match the "
                    "number of arguments";
        }
        if (!error.empty()) return false;
        fields.reserve(obj_.size() - 4);
        for (size_t i = 4; i < obj_.size(); ++i)
            fields.push_back(*obj_.argument(i));
        return true;
    };
    dao::hash_field_values values;
    if (!dao_->hgetdel(handler_.get_db(), *obj_.argument(1), parse, values)) {
        result.create_object().set_error(error.empty() ? dao_->error() :
            error.c_str());
        return true;
    }
    auto& reply = result.create_object();
    reply.set_type(REDIS_OBJ_ARRAY);
    for (const auto& value : values) {
        auto& child = reply.create_child();
        if (value.found) child.set_string(value.value);
        else child.set_null();
    }
    return true;
}
} // namespace pkv
