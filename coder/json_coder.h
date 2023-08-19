//
// Created by zsx on 2023/8/19.
//

#pragma once
#include <yyjson.h>

namespace pkv {

class json_object;
class json_doc;

class json_coder {
public:
    json_coder();
    ~json_coder();

    bool parse(const char* data, size_t len);

    bool is_finished() const {
        return finished_;
    }

private:
    bool finished_;
    std::string type_;
    char* result_;
    json_doc* doc_;
    json_object* root_;

    void create_writer();
};

} // namespace pkv