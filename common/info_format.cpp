#include "info_format.h"
#include "command_catalog.h"
#include "redis_double.h"
#include "redis_integer.h"

namespace pkv {
info_sections::info_sections(const std::vector<const std::string*>& args) {
    const auto defaults = [&] {
        for (const auto* name : {"server", "clients", "memory", "persistence",
            "stats", "replication", "cpu", "module_list", "errorstats",
            "cluster", "keyspace"}) sections_.insert(name);
    };
    if (args.empty()) defaults();
    for (const auto* argument : args) {
        const auto name = command_lower(argument->substr(0,
            argument->find('\0')));
        if (name == "all" || name == "everything") all_ = true;
        else if (name == "default") defaults();
        else sections_.insert(name);
    }
}
bool info_sections::has(const std::string& section) const {
    return all_ || sections_.count(section);
}
void info_header(std::string& out, const char* name) {
    if (!out.empty()) out += "\r\n";
    out += "# "; out += name; out += "\r\n";
}
void info_text(std::string& out, const std::string& key,
    const std::string& value) {
    out += key; out += ':';
    for (char ch : value)
        out += ch == '\r' || ch == '\n' || ch == '\0' ? '?' : ch;
    out += "\r\n";
}
void info_number(std::string& out, const std::string& key, uint64_t value) {
    info_text(out, key, std::to_string(value));
}
void info_server(std::string& out, const process_identity& identity,
    bool cluster, const std::string& address) {
    info_header(out, "Server");
    info_text(out, "server_name", "pkv");
    info_text(out, "redis_version", "0.0.0");
    info_text(out, "redis_mode", cluster ? "cluster" : "standalone");
    info_text(out, "os", identity.os());
    info_number(out, "arch_bits", sizeof(void*) * 8);
    info_number(out, "process_id", static_cast<uint64_t>(identity.pid()));
    info_text(out, "run_id", identity.run_id());
    const auto uptime = identity.uptime();
    info_number(out, "uptime_in_seconds", uptime);
    info_number(out, "uptime_in_days", uptime / 86400);
    const auto separator = address.find_last_of(':');
    long long port;
    if (separator != std::string::npos &&
        parse_redis_integer(address.substr(separator + 1), port) &&
        port >= 0 && port <= 65535) info_number(out, "tcp_port", port);
    info_text(out, "pkv_compiler", __VERSION__);
}
void info_memory(std::string& out, const process_sample& sample) {
    info_header(out, "Memory");
    if (sample.heap_known) {
        info_number(out, "used_memory", sample.heap);
        info_text(out, "mem_allocator", sample.allocator);
    }
    if (sample.rss_known) info_number(out, "used_memory_rss", sample.rss);
}
void info_cpu(std::string& out, const process_sample& sample) {
    if (!sample.cpu_known) return;
    info_header(out, "CPU");
    info_text(out, "used_cpu_sys", format_redis_double(sample.system));
    info_text(out, "used_cpu_user", format_redis_double(sample.user));
    info_text(out, "used_cpu_sys_children",
        format_redis_double(sample.child_system));
    info_text(out, "used_cpu_user_children",
        format_redis_double(sample.child_user));
}
void info_stats(std::string& out, const server_counters& counters,
    uint64_t log_dropped) {
    info_header(out, "Stats");
    uint64_t commands = 0;
    for (const auto& command : counters.commands) commands += command.calls;
    info_number(out, "total_connections_received", counters.connections);
    info_number(out, "total_commands_processed", commands);
    info_number(out, "total_net_input_bytes", counters.input);
    info_number(out, "total_net_output_bytes", counters.output);
    info_number(out, "total_error_replies", counters.errors);
    info_number(out, "acl_access_denied_auth", counters.acl[0]);
    info_number(out, "acl_access_denied_cmd", counters.acl[1]);
    info_number(out, "acl_access_denied_key", counters.acl[2]);
    info_number(out, "acl_access_denied_channel", counters.acl[3]);
    info_number(out, "acl_access_denied_tls_cert", 0);
    info_number(out, "pkv_acl_log_dropped", log_dropped);
    uint64_t represented = 0;
    for (const auto& error : counters.error_codes) represented += error.second;
    info_number(out, "pkv_errorstats_untracked", counters.errors > represented ?
        counters.errors - represented : 0);
}
void info_commands(std::string& out, const server_counters& counters,
    const std::vector<std::string>& names) {
    info_header(out, "Commandstats");
    for (size_t i = 0; i < names.size(); ++i) {
        const auto& value = counters.commands[i];
        if (!value.calls && !value.rejected && !value.failed) continue;
        info_text(out, "cmdstat_" + names[i], "calls=" +
            std::to_string(value.calls) + ",usec=" +
            std::to_string(value.usec) +
            ",usec_per_call=" + format_redis_double(value.calls ?
                static_cast<double>(value.usec) / value.calls : 0) +
            ",rejected_calls=" + std::to_string(value.rejected) +
            ",failed_calls=" + std::to_string(value.failed));
    }
}
void info_errors(std::string& out, const server_counters& counters) {
    info_header(out, "Errorstats");
    for (const auto& error : counters.error_codes) if (error.second)
        info_text(out, "errorstat_" + error.first,
            "count=" + std::to_string(error.second));
}
} // namespace pkv
