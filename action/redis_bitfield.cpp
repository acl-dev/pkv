#include "stdafx.h"
#include "redis_string.h"
#include "redis_handler.h"
#include "coder/redis_coder.h"
#include "coder/redis_object.h"
#include "common/redis_integer.h"

namespace pkv {
namespace {
bool parse_fields(const redis_object& object,
    std::vector<bitfield_operation>& operations, std::string& error) {
    auto overflow = field_overflow::wrap;
    for (size_t i = 2; i < object.size();) {
        const auto remaining = object.size() - i - 1;
        const char* command = object[i];
        bitfield_operation operation;
        operation.overflow = overflow;
        error = "ERR syntax error";
        if (!strcasecmp(command, "GET") && remaining >= 2)
            operation.command = field_command::get;
        else if (!strcasecmp(command, "SET") && remaining >= 3)
            operation.command = field_command::set;
        else if (!strcasecmp(command, "INCRBY") && remaining >= 3)
            operation.command = field_command::increment;
        else if (!strcasecmp(command, "OVERFLOW") && remaining >= 1) {
            const auto mode = object[i + 1];
            if (!strcasecmp(mode, "WRAP")) overflow = field_overflow::wrap;
            else if (!strcasecmp(mode, "SAT"))
                overflow = field_overflow::saturate;
            else if (!strcasecmp(mode, "FAIL")) overflow = field_overflow::fail;
            else {
                error = "ERR Invalid OVERFLOW type specified"; return false;
            }
            i += 2; continue;
        } else return false;
        if (!parse_field_type(*object.argument(i + 1), operation.sign,
            operation.bits)) {
            error = "ERR Invalid bitfield type. Use something like i16 u8. "
                "Note that u64 is not supported but i64 is.";
            return false;
        }
        if (!parse_field_offset(*object.argument(i + 2), operation.bits,
            operation.offset)) {
            error = "ERR bit offset is not an integer or out of range";
            return false;
        }
        if (operation.command != field_command::get &&
            !parse_redis_integer(*object.argument(i + 3), operation.value)) {
            error = "ERR value is not an integer or out of range";
            return false;
        }
        operations.push_back(operation);
        i += operation.command == field_command::get ? 3 : 4;
    }
    return true;
}
} // namespace

bool redis_string::bitfield(redis_coder& result) {
    std::vector<bitfield_operation> operations;
    std::string error;
    if (!parse_fields(obj_, operations, error)) {
        result.create_object().set_error(error); return true;
    }
    std::vector<bitfield_reply> values;
    const bool readonly = !strcasecmp(obj_.get_cmd(), "BITFIELD_RO");
    if (!dao_->bitfield(handler_.get_db(), *obj_.argument(1), operations,
        readonly, values)) {
        result.create_object().set_error(dao_->error()); return true;
    }
    auto& reply = result.create_object();
    reply.set_type(REDIS_OBJ_ARRAY);
    for (const auto& value : values) {
        if (value.null) reply.create_child().set_null();
        else reply.create_child().set_number(value.value);
    }
    return true;
}

} // namespace pkv
