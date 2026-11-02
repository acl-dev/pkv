#pragma once

#include "db/db.h"
#include <mutex>

namespace pkv {
    class redis_service;
    class worker_statistics;
    class database_pool;
    class move_domain;
    class db_watchers;
    class expiry_service;
    class slave_watcher;
    class cluster_manager;
} // namespace pkv

class master_service : public acl::master_fiber {
public:
    static master_service& get_instance();
    master_service();
    ~master_service() override;

    void close_db();

protected:
    // @override
    void on_accept(acl::socket_stream& conn) override;

    // @override
    void proc_on_listen(acl::server_socket& ss) override;

    // @override
    void proc_pre_jail() override;

    // @override
    void proc_on_init() override;

    // @override
    void proc_on_exit() override;

    // @override
    bool proc_on_sighup(acl::string&) override;

    // @override
    void thread_on_init() override;

private:
    std::mutex lifecycle_lock_;
    bool stopping_ = false;
    void shutdown(int received);
    std::unique_ptr<pkv::move_domain> move_domain_;
    std::unique_ptr<pkv::expiry_service> expiry_;
    pkv::shared_db db_ = nullptr;
    std::shared_ptr<pkv::database_pool> databases_;
    pkv::shared_db create_database(size_t index);
    pkv::redis_service* service_;
    pkv::db_watchers* watchers_;
    pkv::slave_watcher* watcher_ = nullptr;
    pkv::cluster_manager& manager_;

    void run(acl::socket_stream& conn, size_t size,
        pkv::worker_statistics& statistics);
    std::vector<std::string> database_roots() const;
    void prepare_watcher();
    void start_watcher();
    std::vector<size_t> existing_databases() const;
};
