#include "stdafx.h"
#include "redis_handler.h"
#include "redis_service.h"
#include "redis_pubsub.h"
#include "common/connection_output.h"
#include "common/command_catalog.h"
#include <cstring>
#include <limits>
namespace pkv {
std::shared_ptr<pubsub_subscriber> redis_handler::ensure_pubsub() {
    if (pubsub_) return pubsub_;
    const auto connection = connection_;
    auto created = std::make_shared<connection_output>(
        [this](const char* data, size_t size) {
            activity_.writing(size);
            const auto sent = conn_.write(data, size);
            return sent > 0 ? static_cast<size_t>(sent) : 0;
        }, [this](size_t size, bool reply) {
            activity_.sent(size, reply); statistics_->sent(size);
        }, [connection] { connection->request_stop(); },
        std::numeric_limits<size_t>::max());
    auto endpoint = service_.pubsub().attach(
        static_cast<uint64_t>(client_.id()), created, client_.protocol(),
        identity_, [connection](bool after_reply) {
            connection->request_stop(after_reply);
        }, [created](bool subscribed) {
            created->limit(subscribed ? 32ULL * 1024 * 1024 :
                std::numeric_limits<size_t>::max());
        });
    try { created->start(); }
    catch (...) { service_.pubsub().retire(endpoint); throw; }
    std::lock_guard<std::mutex> guard(inspector_mutex_);
    output_ = std::move(created); pubsub_ = std::move(endpoint);
    return pubsub_;
}
bool redis_handler::subscribed() const {
    return pubsub_ && pubsub_->counts().total() != 0;
}
void redis_handler::identity(std::shared_ptr<acl_identity> value) {
    if (pubsub_) output_reply_ = service_.pubsub().synchronize(pubsub_,
        client_.protocol(), value, false);
    identity_ = std::move(value);
}
void redis_handler::reset_pubsub() {
    if (pubsub_) output_reply_ = service_.pubsub().synchronize(pubsub_,
        client_.protocol(), identity_, true);
}
bool redis_handler::pubsub_command_valid(const redis_object& object) {
    const auto name = command_lower(*object.argument(1));
    const auto* entry = command_catalog("pubsub|" + name);
    if (entry && ((entry->arity > 0 && size_t(entry->arity) != object.size())
        || (entry->arity < 0 && size_t(-entry->arity) > object.size()))) {
        transaction_.fail();
        builder_.create_object().set_error("ERR wrong number of arguments "
            "for '" + std::string(entry->name) + "' command"); return false;
    }
    if (!entry) {
        transaction_.fail();
        builder_.create_object().set_error("ERR unknown subcommand '" +
            pubsub_error_token(*object.argument(1)) +
            "'. Try PUBSUB HELP."); return false;
    }
    return true;
}
bool redis_handler::pubsub_permit(const redis_command_spec& command,
    const char* label) {
    if (client_.protocol() != 2 || !subscribed()) return true;
    const auto* name = command.name;
    if ((command.kind == redis_command_kind::pubsub &&
        std::strstr(name, "SUBSCRIBE")) || !std::strcmp(name, "PING") ||
        !std::strcmp(name, "QUIT") || !std::strcmp(name, "RESET")) return true;
    builder_.create_object().set_error("ERR Can't execute '" +
        command_lower(label ? label : name) + "': only (P|S)SUBSCRIBE / "
        "(P|S)UNSUBSCRIBE / PING / QUIT / RESET are allowed in this context");
    return false;
}
bool redis_service::pubsub_handler(redis_handler& handler, const char* command,
    const redis_object& object, redis_coder& result) {
    redis_pubsub action(handler, object);
    return action.exec(command, result);
}
} // namespace pkv
