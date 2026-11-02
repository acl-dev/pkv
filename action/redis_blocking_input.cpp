#include "stdafx.h"
#include "redis_handler.h"
#include "redis_service.h"
#include "common/blocked_client.h"
#include "common/connection_input.h"

namespace pkv {
int redis_handler::read(char* data, size_t capacity) {
    input_accounted_ = bool(input_);
    return input_ ? input_->read(data, capacity) :
        conn_.read(data, capacity, false);
}
void redis_handler::promote_input() {
    if (input_) return;
    std::unique_ptr<connection_input> created(new connection_input(conn_,
        [this](size_t size) {
            activity_.buffered(size); statistics_->received(size);
        }, [this] {
            const auto value = connection_->blocked();
            if (value) value->disconnect();
        }));
    created->start();
    std::lock_guard<std::mutex> lock(inspector_mutex_);
    input_ = std::move(created);
}
bool redis_handler::input_closed() const { return input_ && input_->closed(); }
bool redis_handler::recheck(const redis_object& object, redis_coder& result) {
    if (!acl_permit(object, result)) {
        suppress_completion_ = true; return false;
    }
    if (service_.replica() && service_.replica_read_only()) {
        result.create_object().set_error(
            "READONLY You can't write against a read only replica.");
        suppress_completion_ = true; return false;
    }
    return true;
}
} // namespace pkv
