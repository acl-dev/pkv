#include "stdafx.h"
#include "redis_string.h"
#include "redis_handler.h"
#include "coder/redis_coder.h"
#include "coder/redis_object.h"

namespace pkv {
namespace {
struct bitop_name {
    const char* name;
    bitmap_operation operation;
};
const bitop_name operations[] = {
    {"AND", bitmap_operation::conjunction},
    {"OR", bitmap_operation::disjunction},
    {"XOR", bitmap_operation::exclusive_or},
    {"NOT", bitmap_operation::invert},
    {"DIFF", bitmap_operation::difference},
    {"DIFF1", bitmap_operation::reverse_difference},
    {"ANDOR", bitmap_operation::and_or},
    {"ONE", bitmap_operation::one}
};
} // namespace

bool redis_string::bitop(redis_coder& result) {
    const bitop_name* selected = nullptr;
    for (const auto& value : operations)
        if (!strcasecmp(obj_[1], value.name)) { selected = &value; break; }
    if (!selected) {
        result.create_object().set_error("ERR syntax error"); return true;
    }
    if (!valid_bitop(selected->operation, obj_.size() - 3)) {
        std::string error("ERR BITOP ");
        error += selected->name;
        error += selected->operation == bitmap_operation::invert ?
            " must be called with a single source key." :
            " must be called with at least two source keys.";
        result.create_object().set_error(error); return true;
    }
    std::vector<std::string> sources;
    for (size_t i = 3; i < obj_.size(); ++i)
        sources.push_back(*obj_.argument(i));
    long long length;
    if (!dao_->bitop(handler_.get_db(), selected->operation, sources,
        *obj_.argument(2), length))
        result.create_object().set_error(dao_->error());
    else result.create_object().set_number(length);
    return true;
}

} // namespace pkv
