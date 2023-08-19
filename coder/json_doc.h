//
// Created by zsx on 2023/8/19.
//

#pragma once
#include <yyjson.h>

namespace pkv {

class json_doc {
public:
    json_doc(yyjson_doc* doc);
    json_doc();
    ~json_doc();

public:
    yyjson_mut_doc* get_mut_doc() const {
        return w_doc_;
    }

    yyjson_doc* get_doc() const {
        return r_doc_;
    }

    bool is_mutable() const {
        return w_doc_ != nullptr;
    }

private:
    yyjson_doc* r_doc_;
    yyjson_mut_doc* w_doc_;
};

} // namespace pkv