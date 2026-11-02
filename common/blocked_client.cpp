#include "blocked_client.h"
#include "client_registry.h"

namespace pkv {
client_block_scope::client_block_scope(
    std::shared_ptr<client_connection> connection, key_wait_token token)
    : connection_(std::move(connection)),
    value_(std::make_shared<blocked_client>(std::move(token))) {
    if (!connection_->block(value_)) value_->disconnect();
}
client_block_scope::~client_block_scope() {
    value_->finish(); connection_->clear_block(value_);
}
} // namespace pkv
