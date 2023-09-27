//
// Created by shuxin zheng on 2023/9/15.
//

#pragma once

namespace pkv {

/**
 * @brief Represents a node in a Redis cluster.
 * 
 * This class provides methods to set and get various properties of a cluster node,
 * such as its address, id, type, join time, index, and connected status. It also
 * provides a method to add slots to the node and a method to get the slots in the node.
 */
class cluster_node {
public:
    /**
     * @brief Constructs a cluster node with the given address and maximum number of slots.
     * 
     * @param addr The address of the node.
     */
    explicit cluster_node(const char* addr);

    /**
     * @brief Destroys the cluster node and frees any allocated resources.
     */
    ~cluster_node();

    /**
     * @brief Sets myself lable.
     * @param yes True if it's myself, false or else.
     * @return A reference to the modified cluster_node object.
     */
    cluster_node& set_myself(bool yes);

    /**
     * @brief Sets the id of the node.
     * 
     * @param id The id of the node.
     * @return A reference to the modified cluster_node object.
     */
    cluster_node& set_id(const std::string& id);

    /**
     * @brief Sets the type of the node.
     * 
     * @param type The type of the node.
     * @return A reference to the modified cluster_node object.
     */
    cluster_node& set_type(const std::string& type);

    /**
     * @brief Sets the join time of the node.
     * 
     * @param join_time The join time of the node.
     * @return A reference to the modified cluster_node object.
     */
    cluster_node& set_join_time(long long join_time);

    /**
     * @brief Sets the index of the node.
     * 
     * @param idx The index of the node.
     * @return A reference to the modified cluster_node object.
     */
    cluster_node& set_idx(int idx);

    /**
     * @brief Sets the connected status of the node.
     * 
     * @param connected The connected status of the node.
     * @return A reference to the modified cluster_node object.
     */
    cluster_node& set_connected(bool connected);

    /**
     * @brief Adds the given slots to the node.
     * 
     * @param slots A vector of slot numbers to add.
     */
    void add_slots(const std::vector<size_t> &slots);

public:
    NODISCARD bool is_myself() const {
        return myself_;
    }

    /**
     * @brief Returns the address of the node.
     * 
     * @return The address of the node.
     */
    NODISCARD const std::string& get_addr() const {
        return addr_;
    }

    /**
     * @brief Returns the id of the node.
     * 
     * @return The id of the node.
     */
    NODISCARD const std::string& get_id() const {
        return id_;
    }

    /**
     * @brief Returns the type of the node.
     * 
     * @return The type of the node.
     */
    NODISCARD const std::string& get_type() const {
        return type_;
    }

    /**
     * @brief Returns the join time of the node.
     * 
     * @return The join time of the node.
     */
    NODISCARD long long get_join_time() const {
        return join_time_;
    }

    /**
     * @brief Returns the index of the node.
     * 
     * @return The index of the node.
     */
    NODISCARD int get_idx() const {
        return idx_;
    }

    /**
     * @brief Returns the connected status of the node.
     * 
     * @return The connected status of the node.
     */
    NODISCARD bool is_connected() const {
        return connected_;
    }

    /**
     * @brief Returns the IP address of the node.
     * 
     * @return The IP address of the node.
     */
    NODISCARD const std::string& get_ip() const {
        return ip_;
    }

    /**
     * @brief Returns the port number of the node.
     * 
     * @return The port number of the node.
     */
    NODISCARD int get_port() const {
        return port_;
    }

    /**
     * @brief Returns a vector of pairs representing the slots in the node.
     * 
     * @return A vector of pairs representing the slots in the node.
     */
    NODISCARD std::vector<std::pair<size_t, size_t>> get_slots() const;

private:
    bool myself_;
    std::string id_; /**< The id of the node. */
    std::string addr_; /**< The address of the node. */
    std::string ip_; /**< The IP address of the node. */
    int port_ = 0; /**< The port number of the node. */
    std::string type_ = "unknown"; /**< The type of the node. */
    long long join_time_ = 0; /**< The time of joining the cluster. */
    int idx_ = 0; /**< The index of the node. */
    bool connected_ = true; /**< The connected status of the node. */
    ACL_DLINK* slots_ = nullptr; /**< A pointer to the slots in the node. */
};

using shared_node = std::shared_ptr<cluster_node>;

} // namespace pkv
