//
// Created by shuxin zheng on 2023/9/8.
//

#pragma once
#include "cluster_node.h"

namespace pkv {

/**
 * @brief The cluster_service class is a singleton class that manages the cluster nodes and slots.
 * 
 * It provides functionality to bind to a given address and add slots to the cluster.
 */
class cluster_service : public acl::singleton<cluster_service> {
public:
    /**
     * @brief Default constructor for cluster_service.
     */
    cluster_service() = default;

    /**
     * @brief Default destructor for cluster_service.
     */
    ~cluster_service() = default;

    /**
     * @brief Binds the cluster service to the given address and sets the maximum number of slots.
     * 
     * @param addr The address to bind to.
     * @param max_slots The maximum number of slots.
     * @return true if successful, false otherwise.
     */
    bool bind(const char* addr, size_t max_slots);

    /**
     * @brief Adds slots to the cluster.
     * 
     * @param addr The address of the node.
     * @param slots The slots to add.
     * @return {bool}
     */
    bool add_slots(const std::string& addr, const std::vector<size_t>& slots);

    /**
     * Add one node and its all slots belong.
     * @param node {const acl::redis_node&}
     * @return {bool}
     */
    bool add_node(const acl::redis_node& node);

    /**
     * Get all the slots with all the nodes.
     * @return {const std::vector<shared_node>&}
     */
    NODISCARD const std::vector<shared_node>& get_slots() const {
        return slots_;
    }

    /**
     * Get all the nodes.
     * @return {const std::map<std::string, shared_node>&}
     */
    NODISCARD const std::map<std::string, shared_node>& get_nodes() const {
        return nodes_;
    }

    /**
     * Get the node of the addresss
     * @param addr {const std::string&}
     * @return {shared_node}
     */
    NODISCARD shared_node get_node(const std::string& addr) const;

    /**
     * Update the join time of the node with the address
     * @param addr {const std::string&}
     * @return {bool}
     */
    NODISCARD bool update_join_time(const std::string& addr) const;

    /**
     * Get the node for holding the slot of the given key.
     * @param key {const char*}
     * @param slot {size_t} will store the hash slot of the key.
     * @return {shared_node}
     */
    NODISCARD shared_node get_node(const char* key, size_t& slot);

    /**
     * Hash the given key to get the hash slot of it.
     * @param key {const char*}
     * @return {size_t}
     */
    NODISCARD size_t hash_slot(const char* key) const;

public:
    void show_null_slots() const;

private:
    std::map<std::string, shared_node> nodes_; /**< The map of nodes in the cluster. */
    std::vector<shared_node> slots_; /**< The vector of slots in the cluster. */
    acl::server_socket server_; /**< The server socket for the cluster. */
    size_t max_slots_ = 16384; /**< The maximum number of slots in the cluster. */

public:
    // Get the current time stamp.
    static long long get_stamp();
};

} // namespace pkv
