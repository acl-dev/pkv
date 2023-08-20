//
// Created by zsx on 2023/7/23.
//

#include "stdafx.h"
#include "coder/redis_object.h"
#include "coder/redis_coder.h"
#include "redis_service.h"
#include "redis_key.h"
#include "redis_handler.h"

namespace pkv {

#define EQ  !strcasecmp

redis_handler::redis_handler(redis_service& service, shared_db & db,
     redis_coder& parser, acl::socket_stream& conn)
: service_(service)
, db_(db)
, parser_(parser)
, conn_(conn)
, builder_(parser.get_cache())
, coder_(parser.get_cache())
{
}

bool redis_handler::handle() {
    auto objs = parser_.get_objects();
    if (objs.empty()) {
        return true;
    }

#if 0
    {
	std::string tmp;
	for (size_t i = 0; i < objs.size(); i++) {
		tmp += "+OK\r\n";
	}
	return conn_.write(tmp.c_str(), tmp.size()) == (int) tmp.size();
    }
#endif

    //if (objs.size() >= 20) { printf(">>>objs=%zd\r\n", objs.size()); }

    for (const auto& obj : objs) {
        if (!handle_one(*obj)) {
            return false;
        }
    }

    std::string buf;
    if (!builder_.to_string(buf)) {
        builder_.clear();
        return false;
    }

    builder_.clear();

    if (buf.empty()) {
        return true;
    }

    //if (objs.size() >= 20) { printf("reply len=%zd\r\n", buf.size()); }

    //printf(">>>buf=[%s]\r\n", buf.c_str());
    return conn_.write(buf.c_str(), buf.size()) != -1;
}

bool redis_handler::handle_one(const redis_object &obj) {
    auto cmd = obj.get_cmd();
    if (cmd == nullptr || *cmd == '\0') {
        return true;
    }

    auto handler = service_.get_handler(cmd);
    if (handler != nullptr) {
        return (*handler)(*this, cmd, obj, builder_);
    }

    //printf(">>>%s(%d): cmd=%s\r\n", __func__, __LINE__, cmd);

    // The other commands left.

    if (EQ(cmd, "SCAN")) {
        redis_key redis(*this, obj);
        return redis.scan(scan_key_, builder_);
    } else if (EQ(cmd, "QUIT")) {
        (void) conn_.write("+OK\r\n");
        return false;
    }

    std::string err;
    err.append(cmd).append("not support yet");
    logger_error("cmd=%s not support!", cmd);
    builder_.create_object().set_error(err);
    return true;
}

} // namespace pkv