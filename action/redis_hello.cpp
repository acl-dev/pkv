#include "stdafx.h"
#include "redis_server.h"
#include "redis_handler.h"
#include "redis_service.h"
#include "common/redis_integer.h"
#include <cstring>

namespace pkv {
namespace {
bool option(const std::string& value, const char* name) {
    return value.size() == std::strlen(name) &&
        !strncasecmp(value.data(), name, value.size());
}
std::string diagnostic(const std::string& value) {
    std::string out = value.substr(0, value.find('\0'));
    for (auto& ch : out) if (ch == '\r' || ch == '\n') ch = ' ';
    return out;
}
}
bool redis_server::hello(redis_coder& result) {
    auto& state = handler_.client();
    long long version = state.protocol();
    auto& reply = result.create_object();
    if (obj_.size() > 1) {
        if (!parse_redis_integer(*obj_.argument(1), version)) {
            reply.set_error("ERR Protocol version is not an integer or out "
                "of range"); return true;
        }
        if (version < 2 || version > 3) {
            reply.set_error("NOPROTO unsupported protocol version");
            return true;
        }
    }
    const std::string* name = nullptr;
    const std::string* user = nullptr;
    const std::string* password = nullptr;
    for (size_t i = 2; i < obj_.size(); ++i) {
        const auto& arg = *obj_.argument(i);
        if (option(arg, "SETNAME") && i + 1 < obj_.size()) {
            name = obj_.argument(++i);
            if (!client_state::valid_metadata(*name)) {
                reply.set_error("ERR Client names cannot contain spaces, "
                    "newlines or special characters."); return true;
            }
        } else if (option(arg, "AUTH") && i + 2 < obj_.size()) {
            user = obj_.argument(i + 1);
            password = obj_.argument(i + 2); i += 2;
        } else {
            reply.set_error("ERR Syntax error in HELLO option '" +
                diagnostic(arg) + "'"); return true;
        }
    }
    auto identity = user ? handler_.permissions().authenticate(*user, *password)
        : handler_.identity();
    if (user && !identity) {
        handler_.acl_record("auth", *obj_.argument(0), *user);
        reply.set_error("WRONGPASS invalid username-password pair or user "
            "is disabled."); return true;
    }
    if (!user && !state.authenticated()) {
        reply.set_error("NOAUTH HELLO must be called with the client already "
            "authenticated, otherwise the HELLO <proto> AUTH <user> <pass> "
            "option can be used to authenticate the client and select the "
            "RESP protocol version at the same time"); return true;
    }
    reply.set_type(REDIS_OBJ_ARRAY);
    reply.response_as(redis_object::response_kind::map);
    for (const auto& pair : {std::make_pair("server", "pkv"),
        std::make_pair("version", "0.0.0")}) {
        reply.create_child().set_string(pair.first);
        reply.create_child().set_string(pair.second);
    }
    reply.create_child().set_string("proto");
    reply.create_child().set_number(version);
    reply.create_child().set_string("id");
    reply.create_child().set_number(state.id());
    reply.create_child().set_string("mode");
    reply.create_child().set_string(var_cfg_cluster_mode ?
        "cluster" : "standalone");
    reply.create_child().set_string("role");
    reply.create_child().set_string(handler_.get_service().replica() ?
        "replica" : "master");
    reply.create_child().set_string("modules");
    auto& modules = reply.create_child();
    modules.set_type(REDIS_OBJ_ARRAY);
    if (state.hello(static_cast<unsigned>(version), name,
        user ? &identity->name : nullptr, identity->id) !=
        client_state::status::ok) {
        reply.reset();
        reply.set_error("OOM client metadata allocation failed"); return true;
    }
    handler_.identity(std::move(identity));
    return true;
}
} // namespace pkv
