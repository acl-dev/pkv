#include "stdafx.h"
#include "redis_handler.h"
#include "redis_service.h"

namespace pkv {
bool redis_handler::reject_readonly(const redis_command_spec& command) {
    if (!service_.replica() || !service_.replica_read_only()) return false;
    const auto exec = command.kind == redis_command_kind::exec;
    bool writes = command.writes;
    if (exec && transaction_.active()) {
        for (const auto& args : transaction_.commands()) {
            const auto* queued = service_.get_command(args.front().c_str());
            if (queued && queued->writes) { writes = true; break; }
        }
    }
    if (!writes) return false;
    if (exec) {
        clear_transaction(); clear_watches();
        builder_.create_object().set_error("EXECABORT Transaction "
            "discarded because of: READONLY You can't write against a "
            "read only replica.");
    } else {
        transaction_.fail();
        builder_.create_object().set_error(
            "READONLY You can't write against a read only replica.");
    }
    return true;
}
} // namespace pkv
