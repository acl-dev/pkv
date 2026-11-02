#include "stdafx.h"
#include "redis_server.h"
#include "redis_handler.h"
#include "redis_service.h"
#include "common/command_catalog.h"
#include "common/redis_glob.h"
#include <cstring>

namespace pkv {
namespace {
using arguments = std::vector<const std::string*>;

const command_catalog_entry* available(const redis_service& service,
    const std::string& name) {
    const auto* entry = command_catalog(name);
    if (!entry) return nullptr;
    const auto root = std::string(entry->name).substr(0,
        std::string(entry->name).find('|'));
    return service.get_command(root.c_str()) ? entry : nullptr;
}
void strings(redis_object& array, const std::string& words) {
    array.set_type(REDIS_OBJ_ARRAY);
    array.response_as(redis_object::response_kind::set);
    for (size_t first = 0; first < words.size();) {
        const auto end = words.find(' ', first);
        array.create_child().set_status(words.substr(first, end - first));
        if (end == std::string::npos) break;
        first = end + 1;
    }
}
bool append_wire(redis_object& array, const char* wire, size_t bytes,
    redis_ocache& cache, bool docs) {
    redis_coder parser(cache);
    size_t remaining = bytes;
    parser.update(wire, remaining);
    if (remaining || parser.get_objects().size() != 1) return false;
    parser.get_objects().front()->metadata_response(docs);
    parser.move_to(array); return true;
}
void metadata(redis_object& reply, const redis_service& service,
    const arguments& args, bool docs, redis_ocache& cache) {
    reply.set_type(REDIS_OBJ_ARRAY);
    if (docs) reply.response_as(redis_object::response_kind::map);
    std::vector<std::string> roots;
    arguments names;
    if (args.size() <= 2) {
        roots = service.command_names();
        for (const auto& name : roots) names.push_back(&name);
    } else names.assign(args.begin() + 2, args.end());
    for (const auto* name : names) {
        const auto* entry = available(service, *name);
        if (!entry) {
            if (!docs) reply.create_child().set_null();
            continue;
        }
        if (docs) reply.create_child().set_string(entry->name);
        if (!append_wire(reply, docs ? entry->docs : entry->info,
            docs ? entry->docs_size : entry->info_size, cache, docs)) {
            reply.reset(); reply.set_error("ERR command metadata unavailable");
            return;
        }
    }
}
void listing(redis_object& reply, const redis_service& service,
    const arguments& args) {
    std::string filter, value;
    if (args.size() != 2) {
        if (args.size() != 5 || command_lower(*args[2]) != "filterby") {
            reply.set_error("ERR syntax error"); return;
        }
        filter = command_lower(*args[3]); value = command_lower(*args[4]);
        if (filter != "pattern" && filter != "aclcat" && filter != "module") {
            reply.set_error("ERR syntax error"); return;
        }
    }
    reply.set_type(REDIS_OBJ_ARRAY);
    for (const auto& root : service.command_names()) {
        auto names = command_subcommands(root);
        names.insert(names.begin(), command_lower(root));
        for (const auto& name : names) {
            const auto* entry = available(service, name);
            if (!entry || filter == "module") continue;
            if (filter == "pattern" && !redis_glob_match(value, name)) continue;
            if (filter == "aclcat") {
                const auto categories = " " + std::string(entry->categories)
                    + " ";
                if (categories.find(" @" + value + " ") == std::string::npos)
                    continue;
            }
            reply.create_child().set_string(name);
        }
    }
}
void keys(redis_object& reply, const redis_service& service,
    const arguments& args, bool with_flags) {
    const auto& name = *args[2];
    const auto* entry = name.find('|') == std::string::npos ?
        available(service, name) : nullptr;
    if (entry && !command_subcommands(name).empty() && args.size() > 3)
        entry = args[3]->size() > 64 ? nullptr :
            available(service, name + '|' + *args[3]);
    if (!entry) { reply.set_error("ERR Invalid command specified"); return; }
    bool has_keys = false;
    for (size_t i = 0; i < entry->key_count; ++i)
        if (!std::strstr(entry->keys[i].flags, "not_key")) has_keys = true;
    if (!has_keys) {
        reply.set_error("ERR The command has no key arguments"); return;
    }
    const auto count = args.size() - 2;
    if ((entry->arity > 0 && count != static_cast<size_t>(entry->arity)) ||
        (entry->arity < 0 && count < static_cast<size_t>(-entry->arity))) {
        reply.set_error(
            "ERR Invalid number of arguments specified for command");
        return;
    }
    arguments command(args.begin() + 2, args.end());
    std::vector<command_key_argument> found;
    if (!command_key_arguments(*entry, command, found) || found.empty()) {
        reply.set_error("ERR Invalid arguments specified for command"); return;
    }
    reply.set_type(REDIS_OBJ_ARRAY);
    for (const auto& key : found) {
        auto& item = reply.create_child();
        if (!with_flags) item.set_string(*key.key);
        else {
            item.set_type(REDIS_OBJ_ARRAY);
            item.create_child().set_string(*key.key);
            strings(item.create_child(), key.flags);
        }
    }
}
} // namespace

bool redis_server::command(redis_coder& result) {
    arguments args;
    for (size_t i = 0; i < obj_.size(); ++i) args.push_back(obj_.argument(i));
    auto& reply = result.create_object();
    const auto& service = handler_.get_service();
    const auto sub = args.size() == 1 ? "info" :
        args[1]->size() > 32 ? "" : command_lower(*args[1]);
    if (sub == "info" || sub == "docs")
        metadata(reply, service, args, sub == "docs", handler_.get_cache());
    else if (sub == "list") listing(reply, service, args);
    else if (sub == "count" || sub == "help") {
        if (args.size() != 2) {
            reply.set_error("ERR wrong number of arguments for 'command|" +
                sub + "' command"); return true;
        }
        if (sub == "count") reply.set_number(service.command_count());
        else {
            reply.set_type(REDIS_OBJ_ARRAY);
            for (const auto* text : {"COMMAND: command metadata",
                "COUNT", "INFO [name ...]", "DOCS [name ...]",
                "LIST [FILTERBY PATTERN|ACLCAT|MODULE value]",
                "GETKEYS command [arg ...]",
                "GETKEYSANDFLAGS command [arg ...]"})
                reply.create_child().set_string(text);
        }
    } else if (sub == "getkeys" || sub == "getkeysandflags") {
        if (args.size() < 3)
            reply.set_error("ERR wrong number of arguments for 'command|" +
                sub + "' command");
        else keys(reply, service, args, sub == "getkeysandflags");
    } else reply.set_error("ERR unknown subcommand. Try COMMAND HELP.");
    return true;
}
} // namespace pkv
