#include "stdafx.h"
#include "redis_server.h"
#include "redis_handler.h"
#include "common/client_state.h"
#include <cstring>

namespace pkv {
namespace {
std::string error_token(const std::string& value, size_t limit) {
    const auto nul = value.find('\0');
    std::string out = value.substr(0, std::min(limit, nul));
    for (auto& ch : out) if (ch == '\r' || ch == '\n') ch = ' ';
    return out;
}
bool attribute_is(const std::string& value, const char* expected) {
    return value.size() == std::strlen(expected) &&
        !strncasecmp(value.data(), expected, value.size());
}
} // namespace
bool redis_handler::client_command_valid(const redis_object& object) {
    const auto& name = *object.argument(1);
    const auto* spec = client_subcommand(name);
    if (!spec) {
        transaction_.fail();
        builder_.create_object().set_error("ERR unknown subcommand '" +
            error_token(name, 128) + "'. Try CLIENT HELP.");
        return false;
    }
    if (!spec->valid_arity(object.size())) {
        transaction_.fail();
        builder_.create_object().set_error(
            "ERR wrong number of arguments for 'client|" +
            std::string(spec->name) + "' command");
        return false;
    }
    return true;
}
bool redis_server::client(redis_coder& result) {
    const auto* spec = client_subcommand(*obj_.argument(1));
    auto& reply = result.create_object();
    if (!spec || !spec->valid_arity(obj_.size())) {
        reply.set_error("ERR invalid CLIENT request"); return true;
    }
    auto& state = handler_.client();
    if (!std::strcmp(spec->name, "kill")) return client_kill(reply);
    if (!std::strcmp(spec->name, "unblock")) return client_unblock(reply);
    if (!std::strcmp(spec->name, "info")) return client_info(reply, false);
    if (!std::strcmp(spec->name, "list")) return client_info(reply, true);
    if (!std::strcmp(spec->name, "id")) reply.set_number(state.id());
    else if (!std::strcmp(spec->name, "getname")) {
        if (state.name().empty()) reply.set_null();
        else reply.set_string(state.name());
    } else if (!std::strcmp(spec->name, "help")) {
        reply.set_type(REDIS_OBJ_ARRAY);
        for (const char* text : {"ID", "    Read this connection's ID.",
            "GETNAME", "    Read this connection's name.",
            "SETNAME <name>", "    Set a printable name; empty clears it.",
            "SETINFO <LIB-NAME name | LIB-VER version>",
            "    Record the client library metadata.",
            "INFO", "    Read this connection's information.",
            "LIST [TYPE type | ID id [id ...]]",
            "    Read information about matching public connections.",
            "KILL <addr> | <option value> [option value ...]",
            "    Close connections matching all supplied filters.",
            "UNBLOCK <id> [TIMEOUT | ERROR]",
            "    Interrupt a blocked connection.",
            "HELP", "    Show supported client commands."})
            reply.create_child().set_string(text);
    } else {
        client_state::status status;
        std::string attribute;
        if (!std::strcmp(spec->name, "setname"))
            status = state.set_name(*obj_.argument(2));
        else {
            attribute = *obj_.argument(2);
            const bool version = attribute_is(attribute, "LIB-VER");
            if (!version && !attribute_is(attribute, "LIB-NAME")) {
                reply.set_error("ERR Unrecognized option '" +
                    error_token(attribute, std::string::npos) + "'");
                return true;
            }
            status = state.set_library(version, *obj_.argument(3));
        }
        if (status == client_state::status::ok) reply.set_status("OK");
        else if (status == client_state::status::no_memory)
            reply.set_error("OOM client metadata allocation failed");
        else if (attribute.empty()) reply.set_error(
            "ERR Client names cannot contain spaces, newlines or special "
            "characters.");
        else reply.set_error("ERR " + attribute +
            " cannot contain spaces, newlines or special characters.");
    }
    return true;
}
} // namespace pkv
