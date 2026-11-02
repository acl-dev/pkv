#include "pubsub_frame.h"
#include <stdexcept>
namespace pkv {
namespace {
size_t digits(size_t value) {
    size_t result = 1;
    while (value >= 10) { value /= 10; ++result; }
    return result;
}
void add_size(const std::string& out, size_t& total, size_t size) {
    if (size > out.max_size() - total)
        throw std::length_error("subscription frame too large");
    total += size;
}
void add_bulk(const std::string& out, size_t& total, size_t size) {
    add_size(out, total, size); add_size(out, total, digits(size) + 5);
}
void bulk(std::string& out, const std::string& value) {
    out += '$'; out += std::to_string(value.size()); out += "\r\n";
    out.append(value); out += "\r\n";
}
std::string array(size_t count, int protocol) {
    if (protocol != 2 && protocol != 3)
        throw std::invalid_argument("invalid subscription protocol");
    return std::string(protocol == 3 ? ">" : "*") +
        std::to_string(count) + "\r\n";
}
} // namespace
std::string pubsub_error_token(const std::string& value) {
    auto result = value.substr(0, value.find('\0'));
    for (auto& ch : result) if (ch == '\r' || ch == '\n') ch = ' ';
    return result;
}
std::string pubsub_ack(pubsub_kind kind, bool subscribe,
    const std::string* name, size_t count, int protocol) {
    auto out = array(3, protocol);
    const char* labels[] = {"subscribe", "psubscribe", "ssubscribe"};
    const char* removed[] = {"unsubscribe", "punsubscribe", "sunsubscribe"};
    const auto index = static_cast<size_t>(kind);
    if (index >= 3) throw std::invalid_argument("invalid subscription kind");
    const std::string label(subscribe ? labels[index] : removed[index]);
    auto size = out.size();
    add_bulk(out, size, label.size());
    if (name) add_bulk(out, size, name->size());
    else add_size(out, size, protocol == 3 ? 3 : 5);
    add_size(out, size, digits(count) + 3);
    out.reserve(size);
    bulk(out, label);
    if (name) bulk(out, *name);
    else out += protocol == 3 ? "_\r\n" : "$-1\r\n";
    out += ':'; out += std::to_string(count); out += "\r\n";
    return out;
}
std::string pubsub_message(const std::string& channel,
    const std::string& message, const std::string* pattern,
    bool shard, int protocol) {
    auto out = array(pattern ? 4 : 3, protocol);
    const std::string label(pattern ? "pmessage" :
        shard ? "smessage" : "message");
    auto size = out.size();
    add_bulk(out, size, label.size());
    if (pattern) add_bulk(out, size, pattern->size());
    add_bulk(out, size, channel.size()); add_bulk(out, size, message.size());
    out.reserve(size);
    bulk(out, label);
    if (pattern) bulk(out, *pattern);
    bulk(out, channel); bulk(out, message);
    return out;
}
} // namespace pkv
