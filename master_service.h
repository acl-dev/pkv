#pragma once

#include "db/db.h"

namespace pkv {
    class redis_service;
    class db_watchers;
    class cluster_manager;
} // namespace pkv

class master_service : public acl::master_fiber {
public:
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
    pkv::shared_db db_ = nullptr;
    pkv::redis_service* service_ = nullptr;
    pkv::db_watchers* watchers_ = nullptr;
    pkv::cluster_manager& manager_;

    void run(acl::socket_stream& conn, size_t size);
};
