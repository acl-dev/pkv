#include "stdafx.h"
#include "json/json_define.h"
#include "dao_base.h"

namespace pkv {
namespace dao {

dao_base::dao_base()
: finished_(false)
, expire_(-1)
, result_(nullptr)
, w_doc_(nullptr)
, w_root_(nullptr)
, r_doc_(nullptr)
, r_root_(nullptr)
{
}

dao_base::~dao_base() {
    if (result_) {
        free(result_);
    }

    if (w_doc_) {
        yyjson_mut_doc_free(w_doc_);
    }

    if (r_doc_) {
        yyjson_doc_free(r_doc_);
    }
}

void dao_base::reset() {
    finished_ = false;
    if (result_) {
        free(result_);
        result_ = nullptr;
    }
    if (w_doc_) {
        yyjson_mut_doc_free(w_doc_);
        w_doc_ = nullptr;
    }
}

void dao_base::create_writer() {
    if (w_doc_ == nullptr) {
        w_doc_ = yyjson_mut_doc_new(nullptr);
        w_root_ = yyjson_mut_obj(w_doc_);
        yyjson_mut_doc_set_root(w_doc_, w_root_);
    }
}

yyjson_val* dao_base::read(shared_db& db, const std::string& key) {
    std::string buff;
    if (!db->get(key, buff)) {
        return nullptr;
    }

    r_doc_ = yyjson_read(buff.c_str(), buff.size(), 0);

    if (r_doc_ == nullptr) {
        db->del(key);
        return nullptr;
    }

    r_root_ = yyjson_doc_get_root(r_doc_);
    yyjson_val* val = yyjson_obj_get(r_root_, JSON_TYPE);
    if (val == nullptr) {
        db->del(key);
        return nullptr;
    }

    auto type = yyjson_get_str(val);
    auto len = yyjson_get_len(val);
    if (type && len > 0) {
        type_ = type;
    } else {
        return nullptr;
    }

    val = yyjson_obj_get(r_root_, JSON_EXPIRE);
    if (val == nullptr) {
        db->del(key);
        return nullptr;
    }
    expire_ = (time_t) yyjson_get_sint(val);
    if (expire_ >= 0 && expire_ < time(nullptr)) {
        db->del(key);
        return nullptr;
    }

    return yyjson_obj_get(r_root_, "D");
}

bool dao_base::save(shared_db& db, const std::string& key,
        const std::string& data) {
    return db->set(key, data);
}

} // namespace dao
} // namespace pkv
