#pragma once
#include <cstddef>
#include <string>
namespace pkv {
std::string pubsub_error_token(const std::string& value);
enum class pubsub_kind { channel, pattern, shard };
std::string pubsub_ack(pubsub_kind kind, bool subscribe,
    const std::string* name, size_t count, int protocol);
std::string pubsub_message(const std::string& channel,
    const std::string& message, const std::string* pattern,
    bool shard, int protocol);
} // namespace pkv
