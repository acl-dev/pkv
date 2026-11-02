#pragma once
#include "key_wait.h"
#include <atomic>

namespace pkv {
class client_connection;
class blocked_client {
public:
    enum class state { waiting, executing, timeout, error, disconnected, done };
    explicit blocked_client(const key_wait_token& token) : token_(token) {}
    bool waiting() const { return status_.load() == state::waiting; }
    state status() const { return status_.load(); }
    bool begin() {
        auto expected = state::waiting;
        return status_.compare_exchange_strong(expected, state::executing);
    }
    bool park() {
        auto expected = state::executing;
        return status_.compare_exchange_strong(expected, state::waiting);
    }
    bool cancel(bool error) {
        auto expected = state::waiting;
        if (!status_.compare_exchange_strong(expected,
            error ? state::error : state::timeout)) return false;
        if (const auto token = token_.lock()) token->interrupt();
        return true;
    }
    void disconnect() {
        auto current = status_.load();
        while (current != state::done && current != state::disconnected) {
            if (status_.compare_exchange_weak(current, state::disconnected)) {
                if (const auto token = token_.lock()) token->interrupt();
                return;
            }
        }
    }
    void finish() { status_.store(state::done); }
private:
    std::weak_ptr<key_wait_subscription> token_;
    std::atomic<state> status_{state::waiting};
};
class client_block_scope {
public:
    client_block_scope(std::shared_ptr<client_connection> connection,
        key_wait_token token);
    ~client_block_scope();
    blocked_client& control() { return *value_; }
private:
    std::shared_ptr<client_connection> connection_;
    std::shared_ptr<blocked_client> value_;
};
} // namespace pkv
