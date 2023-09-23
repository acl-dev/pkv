//
// Created by shuxin zheng on 2023/9/8.
//

#pragma once

#include "redis_command.h"

namespace pkv {

<<<<<<< HEAD
class redis_coder;
=======
class redis_ocache;
class redis_coder;
class cluster_node;
>>>>>>> master

class redis_cluster : public redis_command {
public:
	redis_cluster(redis_handler& handler, const redis_object& obj);
	~redis_cluster() override;

    bool exec(const char* cmd, redis_coder& result);

public:
<<<<<<< HEAD
    bool addslots(redis_coder& result);
};

} // namespace pkv
=======
    bool cluster_slots(redis_coder& result);
    bool cluster_addslots(redis_coder& result);
    bool cluster_nodes(redis_coder& result);
    bool cluster_meet(redis_coder& result);

    bool cluster_syncslots(redis_coder& result);

private:
    static void add_node(std::string& buf, const std::string& addr,
        const cluster_node& node);
    static void add_nodes(const std::map<acl::string, acl::redis_node*>& nodes);
    static void build_nodes(redis_coder& result);
    static bool sync_slots(redis_ocache& ocache, const std::string& addr,
            const char* myaddr);
};

} // namespace pkv
>>>>>>> master
