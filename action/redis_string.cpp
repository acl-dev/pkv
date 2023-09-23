//
// Created by shuxin zheng on 2023/7/31.
//

#include "stdafx.h"
#include "dao/json/json_string.h"

#include "redis_handler.h"
#include "redis_string.h"

namespace pkv {

#define EQ  !strcasecmp

redis_string::redis_string(redis_handler& handler, const redis_object &obj)
: redis_command(handler, obj)
{
    dao_ = new dao::json_string;
}

redis_string::~redis_string() {
    delete dao_;
}

struct string_handler {
    const char* cmd;
    bool check_slot;
    bool (redis_string::*func) (redis_coder&);
};

static struct string_handler handlers[] = {
    { "SET",        true,   &redis_string::set      },
    { "GET",        true,   &redis_string::get      },

    { nullptr,      false, nullptr            },
};

bool redis_string::exec(const char* cmd, redis_coder& result) {
    for (int i = 0; handlers[i].cmd != nullptr; i++) {
        if (EQ(cmd, handlers[i].cmd)) {
            if (this->check_cluster_mode(handlers[i].check_slot)) {
                CHECK_AND_REDIRECT(cmd, obj_, var_cfg_service_addr);
            }
            return (this->*(handlers[i].func))(result);
        }
    }

    logger_error("Not support, cmd=%s", cmd);
    return false;
}

bool redis_string::set(redis_coder& result) {
    if (obj_.size() < 3) {
        logger_error("invalid SET params' size=%zd", obj_.size());
        return false;
    }

    auto key = obj_[1];
    if (key == nullptr || *key == 0) {
        logger_error("key null");
        return false;
    }

    auto value = obj_[2];
    if (value == nullptr || *value == 0) {
        logger_error("value null");
        return false;
    }

#if 0
    if (!var_cfg_disable_serialize) {
        std::string buff;
        auto& coder = handler_.get_coder();
        coder.clear();

        coder.create_object().create_child().set_string("string", true) // type
            .create_child().set_number(-1);    // expire time
        coder.create_object().set_string(value);
        coder.to_string(buff);

        //printf("buf=[%s]\n", buff.c_str());

        if (!var_cfg_disable_save) {
            if (!handler_.get_db()->set(key, buff.c_str())) {
                logger_error("db set error, key=%s", key);
                return false;
            }
        }
    }
#else
    if (!var_cfg_disable_save) {
        if (!dao_->set(handler_.get_db(), key, value)) {
            logger_error("db set error, key=%s", key);
            return false;
        }
    }
#endif

    result.create_object().set_status("OK");
    return true;
}

bool redis_string::get(redis_coder& result) {
    if (obj_.size() < 2) {
        logger_error("invalid GET params' size=%zd", obj_.size());
        return false;
    }

    auto key = obj_[1];
    if (key == nullptr || *key == 0) {
        logger_error("key null");
        return false;
    }

#if 0
    std::string buff;
    if (!handler_.get_db()->get(key, buff) || buff.empty()) {
        logger_error("db get error, key=%s", key);
        return false;
    }

    //printf(">>>get key=%s, val=[%s]\n", key, buff.c_str());

    auto& coder = handler_.get_coder();
    coder.clear();

    size_t len = buff.size();
    (void) coder.update(buff.c_str(), len);
    if (len > 0) {
        logger_error("invalid buff in db, key=%s", key);
        return false;
    }

    auto& objs2 = coder.get_objects();
    if (objs2.size() != 2) {
        logger_error("invalid object in db, key=%s, size=%zd", key, objs2.size());
        return false;
    }

    auto o = objs2[1];
    if (o->get_type() != REDIS_OBJ_STRING) {
        logger_error("invalid object type=%d, key=%s", (int) o->get_type(), key);
        return false;
    }

    auto v = o->get_str();
    if (v == nullptr || *v == 0) {
        logger_error("value null, key=%s", key);
        return false;
    }
    result.create_object().set_string(v);
#else
    std::string buff;
    if (!dao_->get(handler_.get_db(), key, buff)) {
        result.create_object().set_string("");
        return true;
    }
    result.create_object().set_string(buff);
#endif

    return true;
}

} // namespace pkv
