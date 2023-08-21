#include "stdafx.h"
#include "json_define.h"
#include "json_string.h"

namespace pkv::dao {

json_string::json_string() : data_(nullptr) {}

bool json_string::to_string(std::string& out) {
    auto res = build();
    if (res && *res) {
        out = res;
        return true;
    } else {
        return false;
    }
}

void json_string::set_string(const char* data) {
    data_ = data;
}

const char* json_string::build() {
    if (this->finished_) {
        return this->result_;
    }

    this->create_writer();

    yyjson_mut_obj_add_str(this->w_doc_, this->w_root_,
            JSON_TYPE, JSON_TYPE_STRING);
    yyjson_mut_obj_add_int(this->w_doc_, this->w_root_, JSON_EXPIRE, -1);
    yyjson_mut_obj_add_str(this->w_doc_, this->w_root_, JSON_DATA, data_);
    this->result_ = yyjson_mut_write(this->w_doc_, 0, nullptr);

    finished_ = true;
    return result_;
}

bool json_string::set(shared_db& db, const std::string& key, const char* data) {
    data_ = data;
    if (build() == nullptr) {
        return false;
    }
    //printf(">>key=%s\r\n", key.c_str());
    return dao_base::save(db, key, this->result_);
}

bool json_string::get(shared_db& db, const std::string& key, std::string& out) {
    auto data = this->read(db, key);
    if (data == nullptr) {
        return false;
    }
    if (strcasecmp(this->type_.c_str(), JSON_TYPE_STRING) != 0) {
        logger_error("invalid type=%s, key=%s", type_.c_str(), key.c_str());
        return false;
    }

    auto type = yyjson_get_type(data);
    if (type != YYJSON_TYPE_STR) {
        logger_error("invalid json_node type=%d", (int) type);
        return false;
    }

    auto v = yyjson_get_str(data);
    auto n = yyjson_get_len(data);
    if (v && n > 0) {
        out.append(v, n);
        return true;
    }
    return false;
}

} // namespace pkv::dao
