//
// Created by shuxin zheng on 2023/9/8.
//

#pragma once

#include "redis_command.h"

namespace pkv {

class redis_coder;
class cluster_node;

class redis_cluster : public redis_command {
public:
	redis_cluster(redis_handler& handler, const redis_object& obj);
	~redis_cluster() override;

    bool exec(const char* cmd, redis_coder& result);

public:
    bool addslots(redis_coder& result);
    bool nodes(redis_coder& result);

private:
    void add_node(std::string& buf, const std::string& addr,
        const cluster_node& node);
};

} // namespace pkv