#include "stdafx.h"
#include <cstring>
#include "redis_handler.h"
#include "redis_service.h"
#include "common/client_memory.h"
#include "common/command_catalog.h"
#include "db/watch_registry.h"
#include "common/connection_input.h"
#include "common/connection_output.h"
#include "common/pubsub.h"

namespace pkv {
void redis_handler::receive(const char* data, size_t& size, size_t capacity) {
    if (!input_accounted_) {
        activity_.received(size, capacity);
        statistics_->received(size);
    }
    std::lock_guard<std::mutex> lock(inspector_mutex_);
    parser_.update(data, size);
}
void redis_handler::clear_input() {
    std::lock_guard<std::mutex> lock(inspector_mutex_);
    parser_.clear();
    activity_.idle_arguments();
}
void redis_handler::clear_watches() {
    std::lock_guard<std::mutex> lock(inspector_mutex_);
    watched_.clear();
}
void redis_handler::clear_transaction() {
    std::lock_guard<std::mutex> lock(inspector_mutex_);
    transaction_.clear();
}
bool redis_handler::start_transaction() {
    std::lock_guard<std::mutex> lock(inspector_mutex_);
    return transaction_.start();
}
transaction_queue::result redis_handler::queue_transaction(
    const redis_object& object) {
    std::lock_guard<std::mutex> lock(inspector_mutex_);
    const auto result = transaction_.append(object);
    activity_.idle_arguments();
    return result;
}
const char* redis_handler::command_label(const redis_object& object,
    const redis_command_spec* spec) const {
    if (!spec) return nullptr;
    if (!std::strcmp(spec->name, "CLIENT")) {
        const auto* argument = object.argument(1);
        const auto* child = argument ? client_subcommand(*argument) : nullptr;
        return child ? child->full_name : nullptr;
    }
    if ((!std::strcmp(spec->name, "COMMAND") ||
        !std::strcmp(spec->name, "CONFIG") ||
        !std::strcmp(spec->name, "CLUSTER") ||
        !std::strcmp(spec->name, "ACL") ||
        !std::strcmp(spec->name, "PUBSUB")) && object.size() > 1) {
        const auto& child = *object.argument(1);
        if (child.size() > 64) return nullptr;
        const auto* entry = command_catalog(
            std::string(spec->name) + "|" + child);
        return entry ? entry->name : nullptr;
    }
    return spec->name;
}
client_runtime redis_handler::observe(const void* context, long long now_ms) {
    return static_cast<const redis_handler*>(context)->inspect(now_ms);
}
client_runtime redis_handler::inspect(long long now_ms) const {
    std::lock_guard<std::mutex> lock(inspector_mutex_);
    auto out = activity_.snapshot();
    const auto memory = parser_.memory();
    out.query_size = memory.partial.bytes;
    out.query_free = memory.partial.spare;
    if (input_) {
        out.query_size += input_->pending();
        out.owned_memory += input_->owned();
    }
    if (pubsub_) {
        const auto counts = pubsub_->counts();
        out.sub = counts.channel; out.psub = counts.pattern;
        out.ssub = counts.shard;
        out.owned_memory += pubsub_->owned() + sizeof(*pubsub_) +
            2 * sizeof(void*);
    }
    if (output_) {
        out.output_frames = output_->frames();
        out.output_size += output_->pending();
        out.output_heap += output_->owned();
        out.owned_memory += output_->owned() + sizeof(*output_) +
            2 * sizeof(void*);
    }
    out.owned_memory += memory.complete.owned + memory.partial.owned;
    for (const auto& item : watched_) {
        out.owned_memory += sizeof(item) + string_heap_bytes(item.first.second);
        if (!item.second->valid(now_ms)) out.dirty_watch = true;
    }
    out.watches = out.dirty_watch ? 0 : watched_.size();
    const auto& commands = executing_ ? *executing_ : transaction_.commands();
    if (executing_ || transaction_.active()) {
        out.multi = static_cast<long long>(commands.size());
        if (executing_) out.owned_memory += sizeof(commands);
        out.owned_memory += commands.capacity() * sizeof(commands.front());
        for (const auto& arguments : commands) {
            out.owned_memory += arguments.capacity() * sizeof(std::string);
            for (const auto& argument : arguments) {
                out.multi_memory += argument.size();
                out.owned_memory += string_heap_bytes(argument);
            }
        }
        if (executing_) out.arguments = out.multi_memory;
    }
    return out;
}
} // namespace pkv
