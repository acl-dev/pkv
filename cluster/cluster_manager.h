//
// Created by shuxin zheng on 2023/9/8.
//

#pragma once
#include "db/db.h"
#include "cluster_node.h"

namespace pkv {

struct pkv_node;
class cluster_client;
using shared_cluster_client = std::shared_ptr<cluster_client>;

/**
 * @brief The cluster_manager class is a singleton class that manages the cluster nodes and slots.
 * 
 * It provides functionality to bind to a given address and add slots to the cluster.
 */
class cluster_manager : public acl::singleton<cluster_manager> {
public:
    /**
     * @brief Default constructor for cluster_manager.
     */
    cluster_manager() = default;

    /**
     * @brief Default destructor for cluster_manager.
     */
    ~cluster_manager() = default;

    /**
     * @brief Binds the cluster service to the given address and sets the maximum number of slots.
     * 
     * @param max_slots The maximum number of slots.
     * @param cluster_mode If the service is in cluster mode.
     * @param save_path The path to dump the nodes info.
     */
    void init(size_t max_slots, bool cluster_mode, const char* save_path,
              const std::string& service_ip, int service_port, int rpc_port,
              shared_db db);

    const char* get_service_ip() const {
        return service_ip_.c_str();
    }

    int get_service_port() const {
        return service_port_;
    }

    int get_rpc_port() const {
        return rpc_port_;
    }

    /**
     * @brief Adds slots to the cluster.
     * 
     * @param addr The address of the node.
     * @param slots The slots to add.
     * @return {shared_node}
     */
    shared_node add_slots(const std::string& addr,
               const std::vector<size_t>& slots, bool master);

    /**
     * Set the current node for me
     * @param node
     */
    void set_me(shared_node node);

    /**
     * Get the current pkv node.
     * @return {shared_node}
     */
    NODISCARD shared_node get_me() const {
        return me_;
    }

    /**
     * Set my master node when I'm a slave node.
     * @param master
     */
    void set_master(shared_node& master);

    /**
     * Get my master node if I'm the slave node.
     * @return {shared_node}
     */
    NODISCARD shared_node get_master() const {
        return master_;
    }

    /**
     * Add one node and its all slots belong.
     * @param node {const acl::redis_node&}
     * @return {bool}
     */
    shared_node add_node(const acl::redis_node& node, bool master);

    /**
     * Set if the current node is in master mode.
     * @param yes
     */
    void set_master_mode(bool yes);

    /**
     * Check if the current node is master.
     * @return {bool}
     */
    NODISCARD bool is_master_mode() const {
        return is_master_;
    }

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
    bool sync_open(const std::string& addr);

public:
    NODISCARD bool save_nodes() const;
    bool load_nodes(const char* filepath);

    void show_null_slots() const;

private:
    std::string save_filepath_;
    bool is_master_ = false; /**< If the current pkv node is one master node */

    std::string service_ip_;
    int service_port_ = -1;
    int rpc_port_ = -1;

    shared_node me_; /**< My node object */
    shared_node master_; /**< My master node */
    std::vector<shared_node> slaves_; /** All my slave nodes */

    std::map<std::string, shared_node> nodes_; /**< The map of nodes in the cluster. */

    std::vector<shared_node> slots_; /**< The vector of slots in the cluster. */
    size_t max_slots_ = 16384; /**< The maximum number of slots in the cluster. */

    shared_db db_;
    shared_cluster_client sync_;

    void add_node(const struct pkv_node& node);

public:
    // Get the current time stamp.
    static long long get_stamp();
};

} // namespace pkv
