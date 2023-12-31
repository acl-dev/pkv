#include "stdafx.h"
#include "json_define.h"
#include "json_key.h"

namespace pkv {
namespace dao {

bool json_key::del(shared_db& db, const std::string& keyname) {
    return db->del(keyname);
}

bool json_key::type(shared_db& db, const std::string& keyname, std::string& out) {
    auto data = this->read(db, keyname);
    if (data == nullptr) {
        return false;
    }
    out = this->type_;
    return true;
}

bool json_key::expire(pkv::shared_db &db, const std::string &keyname, int n) {
    auto data = this->read(db, keyname);
    if (data == nullptr) {
        return false;
    }

    this->expire_ = time(nullptr) + n;
    auto val = yyjson_obj_get(r_root_, JSON_EXPIRE);
    if (val) {
        yyjson_set_sint(val, this->expire_);
    } else {
        return false;
    }

    if (this->result_) {
        free(this->result_);
    }
    size_t len;
    this->result_ = yyjson_write(r_doc_, 0, &len);
    return dao_base::save(db, keyname, this->result_);
}

bool json_key::ttl(pkv::shared_db &db, const std::string &keyname, int &n) {
    auto data = this->read(db, keyname);
    if (data == nullptr) {
        return false;
    }
    if (this->expire_ >= 0) {
        n = (int) (this->expire_ - time(nullptr));
    } else {
        n = -1;
    }
    return true;
}

bool json_key::scan(shared_db& db, db_cursor& cursor,
       std::vector<std::string>& keys, size_t max) {
    return db->scan(cursor, keys, max);
}

} // namespace dao
} // namespace pkv
