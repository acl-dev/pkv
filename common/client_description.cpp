#include "client_description.h"
#include <algorithm>

namespace pkv {
namespace {
void field(std::string& out, const char* name, const std::string& value) {
    out += ' '; out += name; out += '='; out += value;
}
void number(std::string& out, const char* name, unsigned long long value) {
    field(out, name, std::to_string(value));
}
} // namespace
bool append_client_description(std::string& out,
    const client_registry::connection& entry,
    long long now, int type) {
    client_view view;
    if (!entry->read_view(view, now)) return false;
    const auto& meta = view.metadata;
    const auto& run = view.runtime;
    const bool subscribed = run.sub || run.psub || run.ssub;
    if (type >= 0 && subscribed != (type == 1)) return false;
    out += "id="; out += std::to_string(meta.id);
    field(out, "addr", entry->address());
    field(out, "laddr", entry->local_address());
    number(out, "fd", static_cast<unsigned long long>(view.descriptor));
    field(out, "name", meta.name);
    field(out, "age", std::to_string(entry->age(now / 1000)));
    const auto idle = std::max(0LL, now / 1000 - run.last_seen);
    field(out, "idle", std::to_string(idle));
    std::string flags;
    if (subscribed) flags += 'P';
    if (run.blocked) flags += 'b';
    if (run.multi >= 0) flags += 'x';
    if (run.dirty_watch) flags += 'd';
    if (view.closing == unsigned(client_connection::state::after_reply))
        flags += 'c';
    if (view.closing == unsigned(client_connection::state::immediate))
        flags += 'A';
    if (view.unix_socket) flags += 'U';
    field(out, "flags", flags.empty() ? "N" : flags);
    number(out, "db", run.database);
    number(out, "sub", run.sub); number(out, "psub", run.psub);
    number(out, "ssub", run.ssub);
    field(out, "multi", std::to_string(run.multi));
    number(out, "watch", run.watches);
    number(out, "qbuf", run.query_size);
    number(out, "qbuf-free", run.query_free);
    number(out, "argv-mem", run.arguments);
    number(out, "multi-mem", run.multi_memory);
    number(out, "rbs", run.output_capacity);
    number(out, "rbp", run.peak);
    number(out, "obl", run.output_size);
    number(out, "oll", run.output_frames);
    number(out, "omem", run.output_heap);
    number(out, "tot-mem", run.owned_memory);
    field(out, "events", run.writing ? "rw" : "r");
    std::string command(run.command);
    if (command != "NULL") for (auto& ch : command)
        if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
    field(out, "cmd", command);
    field(out, "user", meta.username);
    field(out, "redir", "-1");
    number(out, "resp", meta.protocol);
    field(out, "lib-name", meta.library_name);
    field(out, "lib-ver", meta.library_version);
    number(out, "io-thread", run.worker);
    number(out, "tot-net-in", run.input_bytes);
    number(out, "tot-net-out", run.output_bytes);
    number(out, "tot-cmds", run.commands);
    out += '\n';
    return true;
}
} // namespace pkv
