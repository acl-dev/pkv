//
// Created by zsx on 2023/7/23.
//

#pragma once

#include <vector>
#include <map>
#include "coder/redis_coder.h"
#include "db/db.h"
#include "common/transaction_queue.h"
#include "common/client_state.h"
#include "common/client_registry.h"
#include "common/acl_policy.h"
#include "common/server_statistics.h"
#include "common/response_writer.h"

namespace pkv {

class redis_service;
class acl_audit;
struct redis_command_spec;
class database_pool;
class redis_object;
class redis_ocache;
class db_cursor;
class page_workspace;
class transaction_workspace;
class connection_input;
class connection_output;
class pubsub_subscriber;
class redis_pubsub;

class redis_handler {
public:
    explicit redis_handler(redis_service& service,
        const std::shared_ptr<database_pool>& databases,
        redis_coder& parser, acl::socket_stream& conn,
        worker_statistics* statistics = nullptr);

    ~redis_handler();

    bool handle();
    int read(char* data, size_t capacity);
    void promote_input();
    bool input_closed() const;
    bool may_block() const { return executing_ == nullptr; }
    client_registry::connection connection() const { return connection_; }
    void disconnected() { quit_ = true; suppress_completion_ = true; }
    void blocked_time(uint64_t usec) { blocked_usec_ += usec; }
    bool recheck(const redis_object& object, redis_coder& result);
    void receive(const char* data, size_t& size, size_t capacity);
    void receive_capacity(size_t capacity) { activity_.receive_capacity(capacity); }
    void clear_input();

    NODISCARD redis_ocache& get_cache() const {
        return parser_.get_cache();
    }

    NODISCARD redis_coder& get_coder() {
        return coder_;
    }

    NODISCARD shared_db& get_db() {
        return db_;
    }

    const redis_service& get_service() const { return service_; }
    size_t selected_database() const { return selected_; }
    client_state& client() { return client_; }
    size_t database_count() const;
    database_pool& databases() const { return *databases_; }
    shared_db database(size_t index);
    std::vector<std::pair<size_t, shared_db>> info_databases();
    bool select_database(size_t index);
    redis_service& config_service() { return service_; }
    acl_audit& audit();
    acl_policy& permissions();
    void acl_record(const char* reason, const std::string& object,
        const std::string& username) noexcept;
    const std::shared_ptr<acl_identity>& identity() const { return identity_; }
    void identity(std::shared_ptr<acl_identity> value);
    bool subscribed() const;
private:
    friend class redis_list;
    friend class redis_hash;
    friend class redis_zset;
    friend class redis_set;
    friend class redis_string;
    friend class redis_stream;
    db::page_lock_scope* page_pipeline_scope_ = nullptr;
    const redis_command_spec* last_command_ = nullptr;
    friend class transaction_workspace;
    friend class redis_pubsub;
    redis_service& service_;
    std::shared_ptr<database_pool> databases_;
    shared_db db_;
    size_t selected_ = 0;
    client_state client_;
    std::shared_ptr<acl_identity> identity_;
    client_activity activity_;
    client_registry::connection connection_;
    size_t workspace_database_ = 0;
    shared_db workspace_source_;
    std::shared_ptr<page_workspace> workspace_;
    redis_coder& parser_;
    acl::socket_stream& conn_;
    redis_coder builder_;
    redis_coder coder_;
    response_buffer_cache response_buffer_;
    bool quit_ = false;
    bool input_accounted_ = false;
    std::unique_ptr<connection_input> input_;
    std::shared_ptr<connection_output> output_;
    std::shared_ptr<pubsub_subscriber> pubsub_;
    uint64_t output_reply_ = 0;
    std::shared_ptr<pubsub_subscriber> ensure_pubsub();
    void reset_pubsub();
    bool pubsub_command_valid(const redis_object& object);
    bool pubsub_permit(const redis_command_spec& command, const char* label);
    transaction_queue transaction_;
    mutable std::mutex inspector_mutex_;
    const std::vector<std::vector<std::string>>* executing_ = nullptr;
    class execution_view;
    static client_runtime observe(const void* context, long long now_ms);
    client_runtime inspect(long long now_ms) const;
    const char* command_label(const redis_object& object,
        const redis_command_spec* spec) const;
    void clear_watches();
    void clear_transaction();
    bool start_transaction();
    transaction_queue::result queue_transaction(const redis_object& object);
    std::map<std::pair<size_t, std::string>, watch_token> watched_;

    bool acl_permit(const redis_object& object, redis_coder& result,
        bool queued = false);
    bool handle_one(const redis_object& obj);
    bool send_pending();
    bool reject_readonly(const redis_command_spec& command);
    bool client_command_valid(const redis_object& obj);
    bool config_command_valid(const redis_object& obj);
    bool acl_command_valid(const redis_object& obj);
    bool execute_one(const redis_object& obj, redis_coder& result,
        const redis_command_spec* spec = nullptr);
    bool transaction_control(const char* cmd);
    bool execute_transaction();
    bool watch_keys(const redis_object& object);
    bool watches_valid() const;
    bool route_queued_command(const redis_object& object);
    bool route_transaction_keys(
        const std::vector<const std::string*>& keys, const std::string*& first);
    bool route_transaction_owner(const std::string* first);
    bool route_transaction();
    worker_statistics* statistics_ = nullptr;
    size_t root_statistic_ = 0;
    bool root_executed_ = false;
    bool suppress_completion_ = false;
    uint64_t blocked_usec_ = 0;
    bool transaction_acl_failure_ = false;
    size_t statistic(const redis_object& object,
        const redis_command_spec* spec) const;
    void completed(size_t metric, uint64_t start, const redis_coder& result,
        size_t before, bool observed = false);
    void errors(const redis_coder& result, size_t before);
    void record_error(const redis_object& reply);

};

} // namespace pkv
