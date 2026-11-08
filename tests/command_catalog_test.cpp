#include "stdafx.h"
#include "action/redis_service.h"
#include "action/redis_string.h"
#include "action/redis_hash.h"
#include "action/redis_list.h"
#include "common/command_catalog.h"
#include "coder/redis_coder.h"
#include "coder/redis_ocache.h"
#include <cstdlib>
#include <iostream>
#include <thread>

using namespace pkv;
static void require(bool ok, const char* text) {
    if (!ok) { std::cerr << text << '\n'; std::exit(1); }
}
static void verify(const redis_service& service) {
    redis_ocache cache(32);
    const auto names = service.command_names();
    require(names.size() == service.command_count(), "registry count");
    size_t string_commands = 0, hash_commands = 0, list_commands = 0;
    using direct_handler = bool (*)(redis_handler&, const char*,
        const redis_object&, redis_coder&);
    for (const auto& name : names) {
        const auto* entry = command_catalog(name);
        const auto* spec = service.get_command(name.c_str());
        const auto string = redis_string::bind(name.c_str());
        const auto hash = redis_hash::bind(name.c_str());
        const auto list = redis_list::bind(name.c_str());
        string_commands += bool(string); hash_commands += bool(hash);
        list_commands += bool(list);
        if (string || hash || list) require(spec && spec->handler &&
            !spec->handler.target<direct_handler>(),
            "family command retained generic linear dispatcher");
        require(entry && spec && static_cast<size_t>(entry->arity < 0 ?
            -entry->arity : entry->arity) == spec->min_args,
            "registry/catalog arity mismatch");
        require(spec->writes == (std::strstr(entry->categories, "@write") !=
            nullptr), "cached write classification");
        auto mixed = command_lower(name);
        require(service.get_command(mixed.c_str()) == spec,
            "lowercase registry lookup");
        for (size_t i = 0; i < mixed.size(); i += 2) mixed[i] = name[i];
        require(service.get_command(mixed.c_str()) == spec,
            "mixed-case registry lookup");
        for (unsigned byte = 128; byte < 256; ++byte) {
            const auto binary = name + static_cast<char>(byte);
            require(!service.get_command(binary.c_str()),
                "binary suffix matched registered command");
        }
        redis_coder parser(cache);
        size_t remaining = entry->info_size;
        parser.update(entry->info, remaining);
        require(!remaining && parser.get_objects().size() == 1 &&
            parser.get_objects()[0]->size() == 10 &&
            *parser.get_objects()[0]->argument(0) == command_lower(name),
            "catalog descriptor parse");
        std::string roundtrip;
        require(parser.to_string(roundtrip) && roundtrip ==
            std::string(entry->info, entry->info_size), "catalog roundtrip");
        require(!command_catalog(name + std::string("\0suffix", 7)),
            "catalog truncated a binary command name");
    }
    require(string_commands == 32 && hash_commands == 16 &&
        list_commands == 22,
        "registry/family table binding coverage");
    require(!redis_string::bind("UNKNOWN") && !redis_hash::bind("UNKNOWN") &&
        !redis_list::bind("UNKNOWN"),
        "unknown family command binding");
}
int main() {
    acl::acl_cpp_init();
    redis_service service;
    verify(service);
    require(!command_catalog(std::string(1024 * 1024, 'x')),
        "oversized unknown name");
    std::vector<std::thread> readers;
    for (unsigned i = 0; i < 4; ++i)
        readers.emplace_back([&] { verify(service); });
    for (auto& reader : readers) reader.join();
    std::cout << "Command registry/catalog checks passed\n";
}
