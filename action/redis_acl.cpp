#include "stdafx.h"
#include "redis_server.h"
#include "redis_handler.h"
#include "redis_service.h"
#include "common/command_catalog.h"
#include "common/redis_integer.h"
#include <fstream>
#include <set>

namespace pkv {
namespace {
void array(redis_object& out) { out.set_type(REDIS_OBJ_ARRAY); }
void selector_fields(redis_object& out, const acl_selector& selector) {
    out.response_as(redis_object::response_kind::map);
    out.create_child().set_string("commands");
    out.create_child().set_string(acl_selector_commands(selector));
    out.create_child().set_string("keys");
    out.create_child().set_string(acl_selector_keys(selector));
    out.create_child().set_string("channels");
    out.create_child().set_string(acl_selector_channels(selector));
}
void user_fields(redis_object& out, const acl_user& user) {
    array(out); out.response_as(redis_object::response_kind::map);
    out.create_child().set_string("flags");
    auto& flags = out.create_child(); array(flags);
    flags.response_as(redis_object::response_kind::set);
    flags.create_child().set_string(user.enabled ? "on" : "off");
    if (user.nopass) flags.create_child().set_string("nopass");
    flags.create_child().set_string(user.sanitize ? "sanitize-payload" :
        "skip-sanitize-payload");
    out.create_child().set_string("passwords");
    auto& passwords = out.create_child(); array(passwords);
    for (const auto& password : user.passwords)
        passwords.create_child().set_string(password);
    selector_fields(out, user.selectors.front());
    out.create_child().set_string("selectors");
    auto& selectors = out.create_child(); array(selectors);
    for (size_t i = 1; i < user.selectors.size(); ++i) {
        auto& selector = selectors.create_child(); array(selector);
        selector_fields(selector, user.selectors[i]);
    }
}
bool arity(const std::string& name, size_t count) {
    if (name == "setuser" || name == "deluser") return count >= 3;
    if (name == "getuser") return count == 3;
    if (name == "cat" || name == "genpass") return count == 2 || count == 3;
    if (name == "dryrun") return count >= 4;
    if (name == "log") return count == 2 || count == 3;
    return count == 2;
}
void random_password(redis_object& out, const redis_object& object) {
    long long bits = 256;
    if (object.size() == 3 && !parse_redis_integer(*object.argument(2), bits)) {
        out.set_error("ERR value is not an integer or out of range"); return;
    }
    if (bits < 1 || bits > 4096) {
        out.set_error("ERR ACL GENPASS argument must be the number of bits "
            "for the output password, a positive number up to 4096"); return;
    }
    const auto size = static_cast<size_t>((bits + 3) / 4);
    std::string random(size, '\0'), text(size, '\0');
    std::ifstream source("/dev/urandom", std::ios::binary);
    if (!source.read(&random[0], static_cast<std::streamsize>(size))) {
        out.set_error("ERR secure random source unavailable"); return;
    }
    const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < size; ++i)
        text[i] = hex[static_cast<unsigned char>(random[i]) & 15];
    out.set_string(text);
}
} // namespace
bool redis_server::acl(redis_coder& result) {
    auto& reply = result.create_object();
    const auto name = command_lower(*obj_.argument(1));
    const auto* entry = command_catalog("acl|" + name);
    if (!entry || !arity(name, obj_.size())) {
        reply.set_error("ERR unknown subcommand or wrong number of "
            "arguments for '" + acl_safe_token(*obj_.argument(1)) +
            "'. Try ACL HELP."); return true;
    }
    auto& policy = handler_.permissions();
    if (name == "whoami") reply.set_string(handler_.identity()->name);
    else if (name == "genpass") random_password(reply, obj_);
    else if (name == "setuser") {
        std::vector<const std::string*> rules;
        for (size_t i = 3; i < obj_.size(); ++i)
            rules.push_back(obj_.argument(i));
        const auto identity = policy.find(*obj_.argument(2));
        const auto error = policy.set(*obj_.argument(2), rules);
        if (error.empty()) {
            if (identity) handler_.config_service().pubsub().
                validate_permissions(identity, handler_.client().id());
            reply.set_status("OK");
        } else reply.set_error(error);
    } else if (name == "getuser") {
        const auto identity = policy.find(*obj_.argument(2));
        if (identity) user_fields(reply, *identity->snapshot());
        else reply.set_null();
    } else if (name == "list" || name == "users") {
        array(reply);
        for (const auto& identity : policy.users())
            reply.create_child().set_string(name == "users" ? identity->name :
                acl_describe(*identity, *identity->snapshot()));
    } else if (name == "cat") {
        const auto category = obj_.size() == 3 ? *obj_.argument(2) : "";
        const auto categories = acl_categories();
        if (obj_.size() == 3 && std::find(categories.begin(), categories.end(),
            command_lower(category)) == categories.end())
            reply.set_error("ERR Unknown category '" + acl_safe_token(category)
                + "'");
        else {
            array(reply);
            for (const auto& text : obj_.size() == 2 ? categories :
                acl_commands(category))
                reply.create_child().set_string(text);
        }
    } else if (name == "load" || name == "save")
        return acl_file(reply, name == "save");
    else if (name == "log") return acl_log(reply);
    else if (name == "deluser") return acl_deluser(reply);
    else if (name == "dryrun") return acl_dryrun(reply);
    else if (name == "help") {
        array(reply);
        for (const auto& text : {"CAT [category]",
            "DELUSER username [username]",
            "DRYRUN username command [arg ...]", "GENPASS [bits]",
            "GETUSER username", "LIST", "SETUSER username [rule ...]",
            "USERS", "WHOAMI", "LOAD", "SAVE", "LOG [count | RESET]",
            "HELP"})
            reply.create_child().set_string(text);
    }
    return true;
}
bool redis_server::acl_dryrun(redis_object& reply) {
    auto& policy = handler_.permissions();
    const auto identity = policy.find(*obj_.argument(2));
    if (!identity) {
        reply.set_error("ERR User '" + acl_safe_token(*obj_.argument(2)) +
            "' not found"); return true;
    }
    std::vector<const std::string*> arguments;
    for (size_t i = 3; i < obj_.size(); ++i)
        arguments.push_back(obj_.argument(i));
    auto name = command_lower(*arguments[0]);
    if (!command_subcommands(name).empty() && arguments.size() > 1)
        name += '|' + command_lower(*arguments[1]);
    const auto* entry = arguments[0]->find('|') == std::string::npos ?
        command_catalog(name) : nullptr;
    if (!entry) {
        reply.set_error("ERR Command '" + acl_safe_token(*arguments[0]) +
            "' not found"); return true;
    }
    if ((entry->arity > 0 && size_t(entry->arity) != arguments.size()) ||
        (entry->arity < 0 && size_t(-entry->arity) > arguments.size())) {
        reply.set_error("ERR wrong number of arguments for '" + name +
            "' command"); return true;
    }
    const auto decision = policy.check(*identity, arguments);
    if (decision.allowed()) reply.set_status("OK");
    else {
        const bool key = decision.reason == acl_denial::key;
        const bool channel = decision.reason == acl_denial::channel;
        reply.set_string("User " + identity->name +
            (key || channel ? " has no permissions to access the '" :
                " has no permissions to run the '") + decision.object +
            (key ? "' key" : channel ? "' channel" : "' command"));
    }
    return true;
}
bool redis_server::acl_deluser(redis_object& reply) {
    auto& policy = handler_.permissions();
    std::vector<const std::string*> names;
    std::set<uint64_t> identities;
    for (size_t i = 2; i < obj_.size(); ++i) {
        const auto* name = obj_.argument(i);
        if (*name == "default") {
            reply.set_error("ERR The 'default' user cannot be removed");
            return true;
        }
        names.push_back(name);
        const auto identity = policy.find(*name);
        if (identity) identities.insert(identity->id);
    }
    std::vector<client_registry::connection> targets;
    for (const auto& connection : handler_.get_service().clients().snapshot(
        false)) {
        client_snapshot metadata;
        if (connection->read_metadata(metadata) &&
            identities.count(metadata.user_id)) targets.push_back(connection);
    }
    // All snapshots and reply storage precede revocation and socket shutdown.
    std::string counter(20, '0');
    reply.set_string(counter);
    const auto removed = policy.remove(names);
    size_t count = removed.size(), position = counter.size();
    do {
        counter[--position] = static_cast<char>('0' + count % 10); count /= 10;
    } while (count);
    counter.erase(0, position);
    reply.set_string(counter); reply.set_type(REDIS_OBJ_INTEGER);
    const auto self = handler_.client().id();
    for (const auto& connection : targets)
        connection->request_stop(connection->id() == self);
    return true;
}
} // namespace pkv
