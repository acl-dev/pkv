#pragma once
#include <cstddef>
#include <string>
#include <vector>

namespace pkv {
struct command_key_rule {
    const char* flags;
    int begin;
    bool counted;
    int last_or_count;
    int first;
    int step;
    const char* keyword; // nullptr uses begin as a fixed index.
};
struct command_catalog_entry {
    const char* name;
    long long arity;
    const char* categories;
    const char* info;
    size_t info_size;
    const char* docs;
    size_t docs_size;
    const command_key_rule* keys;
    size_t key_count;
};
struct command_key_argument {
    const std::string* key;
    std::string flags;
};
std::vector<const command_catalog_entry*> command_catalog_entries();
std::string command_lower(std::string value);
const command_catalog_entry* command_catalog(const std::string& name);
std::vector<std::string> command_subcommands(const std::string& name);
// Admission also includes effective targets beyond public key-spec matches.
bool command_key_arguments(const command_catalog_entry& entry,
    const std::vector<const std::string*>& args,
    std::vector<command_key_argument>& out, bool admission = false);
} // namespace pkv
