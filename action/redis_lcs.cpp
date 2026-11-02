#include "stdafx.h"
#include "redis_string.h"
#include "redis_handler.h"

namespace pkv {
bool redis_string::lcs(redis_coder& result) {
    std::vector<const std::string*> arguments;
    for (size_t i = 3; i < obj_.size(); ++i)
        arguments.push_back(obj_.argument(i));
    lcs_options options;
    lcs_result value;
    const char* error = nullptr;
    auto& reply = result.create_object();
    if (!dao_->lcs(handler_.get_db(), *obj_.argument(1), *obj_.argument(2),
        arguments, options, value, error)) {
        reply.set_error(error ? error : dao_->error()); return true;
    }
    if (options.length_only) reply.set_number(value.length);
    else if (!options.indexes) reply.set_string(value.value);
    else {
        reply.set_type(REDIS_OBJ_ARRAY);
        reply.response_as(redis_object::response_kind::map);
        reply.create_child().set_string("matches");
        auto& matches = reply.create_child();
        matches.set_type(REDIS_OBJ_ARRAY);
        for (const auto& match : value.matches) {
            auto& item = matches.create_child();
            item.set_type(REDIS_OBJ_ARRAY);
            auto& first = item.create_child();
            first.set_type(REDIS_OBJ_ARRAY);
            first.create_child().set_number(match.first_begin);
            first.create_child().set_number(match.first_end);
            auto& second = item.create_child();
            second.set_type(REDIS_OBJ_ARRAY);
            second.create_child().set_number(match.second_begin);
            second.create_child().set_number(match.second_end);
            if (options.with_length)
                item.create_child().set_number(
                    match.first_end - match.first_begin + 1);
        }
        reply.create_child().set_string("len");
        reply.create_child().set_number(value.length);
    }
    return true;
}
} // namespace pkv
