#include "stdafx.h"
#include "redis_handler.h"
#include "redis_service.h"
#include "common/blocked_client.h"
#include "common/connection_input.h"
#include <acl-lib/fiber/fiber_hook.h>
#include <climits>
#ifndef _WIN32
#include <sys/ioctl.h>
#endif

namespace pkv {
int redis_handler::read(char* data, size_t capacity) {
    input_accounted_ = bool(input_);
    return input_ ? input_->read(data, capacity) :
        conn_.read(data, capacity, false);
}
bool redis_handler::wait_for_receive(std::unique_ptr<char[]>& buffer, size_t& capacity,
    size_t idle_capacity) {
    if (input_) return true;
    const auto timeout = conn_.get_rw_timeout();
    constexpr int idle_ms = 1000;
    auto* stream = conn_.get_vstream();
    const bool milliseconds = ACL_VSTREAM_IS_MS(stream);
    if ((!milliseconds && timeout > INT_MAX / 1000) ||
        ACL_VSTREAM_IS_US(stream) || ACL_VSTREAM_IS_NS(stream) ||
        acl_vstream_can_read(stream) != 0) return true;
    const auto timeout_ms = timeout <= 0 || milliseconds ? timeout : timeout * 1000;
    if (timeout_ms > 0 && timeout_ms <= idle_ms) return true;

    const auto fd = conn_.sock_handle();
    struct pollfd input = {};
    input.fd = fd;
    input.events = POLLIN;
    if (acl_fiber_poll(&input, 1, idle_ms) != 0) return true;

    // The socket wait owns no buffer pointer, so it is safe to release it now.
    try {
        buffer.reset(new char[idle_capacity]);
        capacity = idle_capacity;
        receive_capacity(capacity);
    } catch (const std::bad_alloc&) {
        // Keep the current receive buffer under allocation pressure.
    }
    {
        std::lock_guard<std::mutex> lock(inspector_mutex_);
        parser_.trim_idle_input();
    }
    // Include the idle wait in the connection's configured read timeout.
    return timeout_ms <= 0 || acl_read_wait_ms(fd, timeout_ms - idle_ms) == 0;
}
bool redis_handler::direct_input_ready() const {
    return !input_ && conn_.readable();
}
bool redis_handler::direct_input_pending(size_t minimum) const {
    if (input_) return false;
#ifdef _WIN32
    u_long pending = 0;
    const auto result = ioctlsocket(conn_.sock_handle(), FIONREAD, &pending);
#else
    int pending = 0;
    const auto result = ioctl(conn_.sock_handle(), FIONREAD, &pending);
#endif
    return result == 0 && pending > 0 && static_cast<size_t>(pending) >= minimum;
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
