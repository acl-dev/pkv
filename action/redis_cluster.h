//
// Created by shuxin zheng on 2023/9/8.
//

#pragma once

#include "redis_command.h"

namespace pkv {

class redis_ocache;
class redis_coder;
class cluster_node;
class cluster_manager;

class redis_cluster : public redis_command {
public:
	redis_cluster(redis_handler& handler, const redis_object& obj);
	~redis_cluster() override;

    bool exec(const char* cmd, redis_coder& result);

public:
    bool cluster_slots(redis_coder& result);
    bool cluster_addslots(redis_coder& result);
    bool cluster_nodes(redis_coder& result);
    bool cluster_meet(redis_coder& result);
    bool cluster_replicate(redis_coder& result);

    bool cluster_syncslots(redis_coder& result);
    bool cluster_addslave(redis_coder& result);

private:
    void add_nodes(const std::map<acl::string, acl::redis_node*>& nodes);
    void build_nodes(redis_coder& result);

    void notify_nodes(redis_ocache& ocache);
    bool link_master(redis_ocache& ocache, acl::socket_stream& conn,
            const std::string& rpc_addr, const std::string& myaddr);

    static void add_node(std::string &buf, const cluster_node &node);
    static bool sync_slots(redis_ocache& ocache, const std::string& addr,
            const char* myaddr);

private:
    static bool read_status(acl::socket_stream& conn, redis_coder& coder);

private:
    cluster_manager& manager_;
};

} // namespace pkv
