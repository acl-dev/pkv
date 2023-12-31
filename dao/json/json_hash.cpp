#include "stdafx.h"
#include "json_define.h"
#include "json_hash.h"

namespace pkv {
namespace dao {

const char* json_hash::build() {
    if (this->finished_) {
        return this->result_;
    }

    this->create_writer();

    yyjson_mut_obj_add_str(this->w_doc_, this->w_root_, JSON_TYPE, JSON_TYPE_HASH);
    yyjson_mut_obj_add_int(this->w_doc_, this->w_root_, JSON_EXPIRE, -1);

    auto data = yyjson_mut_obj(this->w_doc_);
    yyjson_mut_obj_add_val(this->w_doc_, this->w_root_, JSON_DATA, data);

    for (const auto& cit : fields_) {
        yyjson_mut_obj_add_str(this->w_doc_, data, cit.first.c_str(),
                cit.second.c_str());
    }

    this->result_ = yyjson_mut_write(this->w_doc_, 0, nullptr);

    this->finished_ = true;
    return result_;
}

bool json_hash::hset(shared_db& db, const std::string& key, const std::string& name,
        const std::string& value) {

    (void) hgetall(db, key);
    fields_[name] = value;

    if (build() == nullptr) {
        logger_error("Build for hset error, key=%s, name=%s, value=%s",
              key.c_str(), name.c_str(), value.c_str());
        return false;
    }
    //printf(">>>%s<<<\r\n", this->result_);
    return dao_base::save(db, key, this->result_);
}

int json_hash::hdel(shared_db& db, const std::string& key, const std::string& name) {
    if (!hgetall(db, key)) {
        return -1;
    }
    auto it = fields_.find(name);
    if (it == fields_.end()) {
        return 0;
    }

    fields_.erase(it);

    if (fields_.empty()) {
        db->del(key);
    } else if (build() == nullptr) {
        logger_error("Build for hset error, key=%s, name=%s",
              key.c_str(), name.c_str());
        return -1;
    } else if (!dao_base::save(db, key, this->result_)) {
        return -1;
    }

    return 1;
}

// { "T": "H", "E": -1, "D": { "name1": "value1", "name2": "value2" }}

bool json_hash::hget(shared_db& db, const std::string& key, const std::string& name,
        std::string& value) {
    auto data = this->read(db, key);
    if (data == nullptr) {
        logger_error("db read error, key=%s", key.c_str());
        return false;
    }
    if (strcasecmp(type_.c_str(), JSON_TYPE_HASH) != 0) {
        logger_error("Invalid type=%s, key=%s", type_.c_str(), key.c_str());
        return false;
    }

    yyjson_obj_iter iter;
    yyjson_obj_iter_init(data, &iter);

    yyjson_val* vkey, *vval;
    while ((vkey = yyjson_obj_iter_next(&iter))) {
        vval = yyjson_obj_iter_get_val(vkey);
        if (yyjson_equals_str(vkey, name.c_str())) {
            auto v = yyjson_get_str(vval);
            if (v) {
                value = v;
            }
            return true;
        }
    }

    return false;
}

bool json_hash::hgetall(shared_db& db, const std::string& key) {
    fields_.clear();

    auto data = this->read(db, key);
    if (data == nullptr) {
        //logger_error("db read error, key=%s", key.c_str());
        return true;
    }
    if (strcasecmp(type_.c_str(), JSON_TYPE_HASH) != 0) {
        logger_error("Invalid type=%s, key=%s", type_.c_str(), key.c_str());
        return false;
    }

    yyjson_obj_iter iter;
    yyjson_obj_iter_init(data, &iter);

    yyjson_val* vkey, *vval;
    while ((vkey = yyjson_obj_iter_next(&iter))) {
        vval = yyjson_obj_iter_get_val(vkey);
        auto n = yyjson_get_str(vkey);
        auto v = yyjson_get_str(vval);
        if (n && v) {
            fields_[n] = v;
        }
    }

    return true;
}

int json_hash::hmset(shared_db &db, const std::string &key,
     const std::map<std::string, std::string> &fields) {
    (void) hgetall(db, key);
    for (auto field : fields) {
        fields_[field.first] = field.second;
    }

    if (build() == nullptr) {
        logger_error("Build for hmset error, key=%s", key.c_str());
        return false;
    }
    return dao_base::save(db, key, this->result_);
}

bool json_hash::hmget(shared_db &db, const std::string &key,
     const std::vector<std::string> &names,
     std::map<std::string, std::string>& result) {

    (void) hgetall(db, key);
    for (auto name : names) {
        auto it = fields_.find(name);
        if (it != fields_.end()) {
            result[name] = it->second;
        } else {
            result[name] = "";
        }
    }
    return true;
}

} // namespace dao
} // namespace pkv
