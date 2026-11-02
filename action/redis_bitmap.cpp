#include "stdafx.h"
#include "redis_string.h"
#include "redis_handler.h"
#include "coder/redis_coder.h"
#include "coder/redis_object.h"
#include "common/redis_integer.h"
#include "common/redis_string_util.h"

namespace pkv {
namespace {
const char* integer_error = "ERR value is not an integer or out of range";
bool index_unit(const std::string& value, bitmap_range& range) {
    if (!strcasecmp(value.c_str(), "BIT")) range.bits = true;
    else if (strcasecmp(value.c_str(), "BYTE")) return false;
    return true;
}
} // namespace

bool redis_string::bitmap(redis_coder& result) {
    long long offset, bit = 0, previous;
    if (!parse_redis_integer(*obj_.argument(2), offset) || offset < 0 ||
        static_cast<uint64_t>(offset) >=
            static_cast<uint64_t>(redis_string_max) * 8) {
        result.create_object().set_error(
            "ERR bit offset is not an integer or out of range");
        return true;
    }
    const bool write = !strcasecmp(obj_.get_cmd(), "SETBIT");
    if (write && (!parse_redis_integer(*obj_.argument(3), bit) ||
        (bit != 0 && bit != 1))) {
        result.create_object().set_error(
            "ERR bit is not an integer or out of range");
        return true;
    }
    const bool ok = write ? dao_->setbit(handler_.get_db(), *obj_.argument(1),
        offset, static_cast<int>(bit), previous) :
        dao_->getbit(handler_.get_db(), *obj_.argument(1), offset, previous);
    if (ok) result.create_object().set_number(previous);
    else result.create_object().set_error(dao_->error());
    return true;
}

bool redis_string::bitcount(redis_coder& result) {
    bitmap_range range;
    if (obj_.size() != 2 && obj_.size() != 4 && obj_.size() != 5) {
        result.create_object().set_error("ERR syntax error"); return true;
    }
    if (obj_.size() >= 4) {
        if (!parse_redis_integer(*obj_.argument(2), range.start) ||
            !parse_redis_integer(*obj_.argument(3), range.end)) {
            result.create_object().set_error(integer_error); return true;
        }
        range.end_given = true;
        if (obj_.size() == 5 && !index_unit(*obj_.argument(4), range)) {
            result.create_object().set_error("ERR syntax error"); return true;
        }
    }
    long long count;
    if (!dao_->bitcount(handler_.get_db(), *obj_.argument(1), range, count))
        result.create_object().set_error(dao_->error());
    else result.create_object().set_number(count);
    return true;
}

bool redis_string::bitpos(redis_coder& result) {
    bitmap_range range;
    long long bit, position;
    if (!parse_redis_integer(*obj_.argument(2), bit)) {
        result.create_object().set_error(integer_error); return true;
    }
    if (bit != 0 && bit != 1) {
        result.create_object().set_error(
            "ERR The bit argument must be 1 or 0.");
        return true;
    }
    if (obj_.size() > 6) {
        result.create_object().set_error("ERR syntax error"); return true;
    }
    if (obj_.size() >= 4) {
        if (!parse_redis_integer(*obj_.argument(3), range.start)) {
            result.create_object().set_error(integer_error); return true;
        }
        // Redis checks the unit before the optional end integer.
        if (obj_.size() == 6 && !index_unit(*obj_.argument(5), range)) {
            result.create_object().set_error("ERR syntax error"); return true;
        }
        if (obj_.size() >= 5) {
            if (!parse_redis_integer(*obj_.argument(4), range.end)) {
                result.create_object().set_error(integer_error); return true;
            }
            range.end_given = true;
        }
    }
    if (!dao_->bitpos(handler_.get_db(), *obj_.argument(1),
        static_cast<int>(bit), range, position))
        result.create_object().set_error(dao_->error());
    else result.create_object().set_number(position);
    return true;
}

} // namespace pkv
