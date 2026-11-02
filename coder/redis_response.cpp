#include "redis_object.h"

namespace pkv {
redis_object& redis_object::set_raw(std::string&& data) {
    type_ = REDIS_OBJ_STRING;
    buf_ = std::move(data);
    return response_as(response_kind::raw);
}

bool redis_object::to_resp(std::string& out, unsigned protocol) const {
    if (response_protocol_) protocol = response_protocol_;
    if (protocol != 2 && protocol != 3) return false;
    const auto count = objs_ ? objs_->size() : 0;
    if (response_kind_ == response_kind::raw) {
        if (type_ != REDIS_OBJ_STRING || count) return false;
        out += buf_; return true;
    }
    if (count || type_ == REDIS_OBJ_ARRAY || type_ == REDIS_OBJ_EMPTY) {
        const auto kind = protocol == 3 ? response_kind_ : response_kind::plain;
        const bool map = kind == response_kind::map;
        const bool pairs = kind == response_kind::pairs;
        if ((map || pairs) && count % 2) return false;
        const char prefix = map ? '%' : kind == response_kind::set ? '~' : '*';
        out += prefix;
        out += std::to_string(map || pairs ? count / 2 : count);
        out += "\r\n";
        for (size_t i = 0; i < count; ++i) {
            if (pairs && i % 2 == 0) out += "*2\r\n";
            if (!(*objs_)[i]->to_resp(out, protocol)) return false;
        }
        return true;
    }
    if (protocol == 3) {
        if (type_ == REDIS_OBJ_STRING &&
            response_kind_ == response_kind::verbatim) {
            out += '='; out += std::to_string(buf_.size() + 4);
            out += "\r\ntxt:"; out += buf_; out += "\r\n"; return true;
        }
        if (type_ == REDIS_OBJ_NULL_STRING || type_ == REDIS_OBJ_NULL_ARRAY) {
            out += "_\r\n"; return true;
        }
        if (type_ == REDIS_OBJ_STRING &&
            response_kind_ == response_kind::real) {
            out += ','; out += buf_; out += "\r\n"; return true;
        }
    }
    return to_string(out);
}

void redis_object::metadata_response(bool docs) {
    if (!objs_) return;
    if (docs) {
        response_as(response_kind::map);
        return;
    }
    if (objs_->size() != 10) return;
    for (const auto i : {2, 6, 7, 8})
        (*objs_)[i]->response_as(response_kind::set);
    auto& specs = *(*objs_)[8];
    if (specs.objs_) for (auto* spec : *specs.objs_) {
        spec->response_as(response_kind::map);
        if (!spec->objs_) continue;
        for (size_t i = 0; i + 1 < spec->objs_->size(); i += 2) {
            auto& name = *(*spec->objs_)[i];
            auto& value = *(*spec->objs_)[i + 1];
            if (name.buf_ == "flags") value.response_as(response_kind::set);
            else if (name.buf_ == "begin_search" || name.buf_ == "find_keys") {
                value.response_as(response_kind::map);
                if (value.objs_) for (size_t j = 0;
                    j + 1 < value.objs_->size(); j += 2)
                    if ((*value.objs_)[j]->buf_ == "spec")
                        (*value.objs_)[j + 1]->response_as(response_kind::map);
            }
        }
    }
    auto& children = *(*objs_)[9];
    if (!children.objs_ || children.objs_->empty())
        children.response_as(response_kind::set);
    else for (auto* child : *children.objs_) child->metadata_response(false);
}
} // namespace pkv
