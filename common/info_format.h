#pragma once
#include <set>
#include <string>
#include <vector>
#include "server_statistics.h"
#include "process_info.h"

namespace pkv {
class info_sections {
public:
    explicit info_sections(const std::vector<const std::string*>& args);
    bool has(const std::string& section) const;
private:
    bool all_ = false;
    std::set<std::string> sections_;
};
void info_header(std::string& out, const char* name);
void info_text(std::string& out, const std::string& key,
    const std::string& value);
void info_number(std::string& out, const std::string& key, uint64_t value);
void info_server(std::string& out, const process_identity& identity,
    bool cluster, const std::string& address);
void info_memory(std::string& out, const process_sample& sample);
void info_cpu(std::string& out, const process_sample& sample);
void info_stats(std::string& out, const server_counters& counters,
    uint64_t log_dropped);
void info_commands(std::string& out, const server_counters& counters,
    const std::vector<std::string>& names);
void info_errors(std::string& out, const server_counters& counters);
} // namespace pkv
