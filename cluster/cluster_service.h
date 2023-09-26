//
// Created by shuxin zheng on 2023/9/26.
//

#pragma once
#include "cluster_manager.h"

namespace pkv {

using shared_server = std::shared_ptr<acl::server_socket>;
class slave_watcher;

class cluster_service {
public:
    /**
     * @brief Construct a new cluster service object
     * 
     */
    cluster_service(slave_watcher& watcher);

    /**
     * @brief Destroy the cluster service object
     * 
     */
    ~cluster_service() = default;

    /**
     * @brief Bind the server to the specified address
     * 
     * @param addr The address to bind to
     * @return true if the server is successfully bound, false otherwise
     */
    bool bind(const char* addr);

    /**
     * @brief Run the cluster service
     * 
     * @note This function does not return
     */
    NORETURN void run();

private:
    shared_server server_ = std::make_shared<acl::server_socket>
	    (128, acl::OPEN_FLAG_REUSEPORT);
    slave_watcher& watcher_;
};

} // namespace pkv
