#include "stdafx.h"
#include "redis_handler.h"
#include "redis_service.h"

namespace pkv {
size_t redis_handler::statistic(const redis_object& object,
    const redis_command_spec* spec) const {
    if (spec && spec->name[0] != 'C' && std::strcmp(spec->name, "ACL") &&
        std::strcmp(spec->name, "PUBSUB"))
        return spec->statistic;
    return service_.statistics().index(command_label(object, spec));
}
void redis_handler::completed(size_t metric, uint64_t start,
    const redis_coder& result, size_t before, bool observed) {
    if (!executing_ && suppress_completion_) return;
    const bool failed = (!executing_ && transaction_acl_failure_) ||
        (result.get_objects().size() > before &&
        result.get_objects().back()->get_type() == REDIS_OBJ_ERROR);
    auto duration = statistics_time_us() - start;
    if (!executing_) duration -= std::min(duration, blocked_usec_);
    if (observed) statistics_->complete_observed(metric, duration, failed);
    else statistics_->complete(metric, duration, failed);
    root_executed_ = true;
    activity_.complete();
}
void redis_handler::errors(const redis_coder& result, size_t before) {
    for (size_t i = before; i < result.get_objects().size(); ++i) {
        const auto& reply = *result.get_objects()[i];
        if (reply.get_type() == REDIS_OBJ_ERROR) record_error(reply);
    }
}

void redis_handler::record_error(const redis_object& reply) {
    const auto& text = reply.get_buf();
    char code[33]{};
    size_t count = 0;
    bool valid = true;
    for (char ch : text) {
        if (ch == ' ') break;
        if (count == 32) { valid = false; break; }
        if ((ch < 'A' || ch > 'Z') && (ch < '0' || ch > '9') &&
            ch != '_') valid = false;
        code[count++] = ch;
    }
    statistics_->error(valid && count ? code : "ERR");
}
} // namespace pkv
