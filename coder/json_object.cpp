//
// Created by zsx on 2023/8/19.
//

#include "stdafx.h"
#include "json_doc.h"
#include "json_object.h"

namespace pkv {

json_object::json_object(yyjson_val* obj) : obj_(obj), obj_mut_(nullptr) {}

json_object::json_object(yyjson_mut_val* obj) : obj_(nullptr), obj_mut_(obj) {}

json_object::~json_object() {
    for (auto o : objs_tmp_) {
        delete o;
    }
}

bool json_object::get_str(std::string& out) const {
    if (obj_) {
        auto str = yyjson_get_str(obj_);
        auto len = yyjson_get_len(obj_);
        if (str && len > 0) {
            out.assign(str, len);
            return true;
        }
        return false;
    } else if (obj_mut_) {
        auto str = yyjson_mut_get_str(obj_mut_);
        auto len = yyjson_mut_get_len(obj_mut_);
        if (str && len > 0) {
            out.assign(str, len);
            return true;
        }
        return false;
    } else {
        return false;
    }
}

json_object *json_object::get_obj(const char *tag) {
    auto val = yyjson_obj_get(obj_, tag);
    if (val == nullptr) {
        return nullptr;
    }

    auto obj = new json_object(val);
    objs_tmp_.push_back(obj);
    return obj;
}

json_object *json_object::first_child() {
    yyjson_obj_iter_init(obj_, &iter_);
    return next_child();
}

json_object *json_object::next_child() {
    auto val = yyjson_obj_iter_next(&iter_);
    if (val == nullptr) {
        return nullptr;
    }
    auto obj = new json_object(val);
    objs_tmp_.push_back(obj);
    return obj;
}

json_object *json_object::iter_val() {
    auto val = yyjson_obj_iter_get_val(obj_);
    if (val == nullptr) {
        return nullptr;
    }
    auto obj = new json_object(val);
    objs_tmp_.push_back(obj);
    return obj;
}

} // namespace pkv