//
// Created by zsx on 2023/8/19.
//

#pragma once
#include <yyjson.h>

namespace pkv {

class json_doc;

class json_object {
public:
    json_object(yyjson_val* obj);
    json_object(yyjson_mut_val* obj);

    bool get_str(std::string& out) const;
    json_object* get_obj(const char* tag);

    json_object* first_child();
    json_object* next_child();
    json_object* iter_val();

public:
    yyjson_val* get_obj() const {
        return obj_;
    }

    yyjson_mut_val* get_mut_obj() const {
        return obj_mut_;
    }

private:
    ~json_object();

private:
    yyjson_val* obj_;
    yyjson_mut_val* obj_mut_;
    yyjson_obj_iter iter_;

    std::vector<json_object*> objs_tmp_;
};

} // namespace pkv
