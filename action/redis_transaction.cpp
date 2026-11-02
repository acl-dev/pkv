#include "stdafx.h"
#include "redis_handler.h"
#include "redis_service.h"
#include "coder/redis_object.h"
#include "coder/record_codec.h"
#include "slave/replica_scope.h"
#include "transaction_workspace.h"
#include "common/key_wait_batch.h"

namespace pkv {
class redis_handler::execution_view {
public:
    execution_view(redis_handler& owner,
        std::vector<std::vector<std::string>>& commands) : owner_(owner) {
        std::lock_guard<std::mutex> lock(owner_.inspector_mutex_);
        commands = owner_.transaction_.take();
        owner_.executing_ = &commands;
    }
    ~execution_view() {
        std::lock_guard<std::mutex> lock(owner_.inspector_mutex_);
        owner_.executing_ = nullptr;
    }
private:
    redis_handler& owner_;
};

bool redis_handler::transaction_control(const char* cmd) {
    if (!strcasecmp(cmd, "MULTI")) {
        if (start_transaction()) builder_.create_object().set_status("OK");
        else builder_.create_object().set_error(
            "ERR MULTI calls can not be nested");
    } else if (!strcasecmp(cmd, "DISCARD")) {
        if (!transaction_.active()) builder_.create_object().set_error(
            "ERR DISCARD without MULTI");
        else {
            clear_transaction(); clear_watches();
            builder_.create_object().set_status("OK");
        }
    } else if (!strcasecmp(cmd, "EXEC")) {
        return execute_transaction();
    } else if (!strcasecmp(cmd, "RESET")) {
        if (!select_database(0)) {
            builder_.create_object().set_error("ERR storage operation failed");
            return true;
        }
        clear_transaction(); clear_watches();
        identity_ = service_.permissions().find("default");
        client_.reset(service_.permissions().default_login());
        reset_pubsub();
        builder_.create_object().set_status("RESET");
    } else if (!strcasecmp(cmd, "QUIT")) {
        builder_.create_object().set_status("OK"); quit_ = true;
    } else return false;
    return true;
}

bool redis_handler::watches_valid() const {
    const auto now = unix_time_ms();
    for (const auto& item : watched_)
        if (!item.second->valid(now)) return false;
    return true;
}

bool redis_handler::watch_keys(const redis_object& object) {
    if (transaction_.active()) {
        builder_.create_object().set_error(
            "ERR WATCH inside MULTI is not allowed");
        return true;
    }
    try {
        if (route_queued_command(object)) return true;
        if (!watches_valid()) {
            builder_.create_object().set_status("OK"); return true;
        }
        std::vector<std::string> keys;
        for (size_t i = 1; i < object.size(); ++i) {
            const auto& key = *object.argument(i);
            if (!watched_.count(std::make_pair(selected_, key)))
                keys.push_back(key);
        }
        std::sort(keys.begin(), keys.end());
        keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
        if (!keys.empty()) {
            auto pending = watched_;
            std::vector<watch_token> tokens;
            if (db_->watch(keys, tokens) != db::batch_status::ok) {
                builder_.create_object().set_error("ERR cannot watch keys");
                return true;
            }
            for (size_t i = 0; i < keys.size(); ++i)
                pending.emplace(std::make_pair(selected_, std::move(keys[i])),
                    std::move(tokens[i]));
            {
                std::lock_guard<std::mutex> lock(inspector_mutex_);
                watched_.swap(pending);
            }
        }
        builder_.create_object().set_status("OK");
    } catch (const std::bad_alloc&) {
        builder_.create_object().set_error("OOM cannot watch keys");
    }
    return true;
}

bool redis_handler::execute_transaction() {
    if (!transaction_.active()) {
        builder_.create_object().set_error("ERR EXEC without MULTI");
        return true;
    }
    if (transaction_.failed()) {
        clear_transaction(); clear_watches();
        builder_.create_object().set_error(
            "EXECABORT Transaction discarded because of previous errors.");
        return true;
    }
    try {
        if (route_transaction()) {
            clear_transaction(); clear_watches(); return true;
        }
    } catch (const std::bad_alloc&) {
        clear_transaction(); clear_watches();
        builder_.create_object().set_error("OOM cannot route transaction");
        return true;
    }
    if (!watches_valid()) {
        clear_transaction(); clear_watches();
        builder_.create_object().set_type(REDIS_OBJ_NULL_ARRAY);
        return true;
    }
    clear_watches();
    std::vector<std::vector<std::string>> commands;
    execution_view inspecting(*this, commands);
    key_wait_batch readiness;
    replica_scope replication(service_.replication_watcher());
    transaction_workspace workspace(*this, commands);
    auto& array = builder_.create_object();
    array.set_type(REDIS_OBJ_ARRAY);
    for (const auto& arguments : commands) {
        redis_coder request(get_cache()), response(get_cache());
        auto& object = request.create_object();
        object.set_type(REDIS_OBJ_ARRAY);
        for (const auto& argument : arguments)
            object.create_child().set_string(argument);
        execute_one(object, response);
        if (response.get_objects().size() != 1) {
            response.clear();
            response.create_object().set_error("ERR invalid command reply");
        }
        for (auto* reply : response.get_objects())
            reply->response_protocol(client_.protocol());
        response.move_to(array);
    }
    if (!workspace.commit()) {
        array.reset();
        array.set_error("ERR EXEC storage commit failed");
    }
    return true;
}

} // namespace pkv
