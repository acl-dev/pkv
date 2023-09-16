//
// Created by shuxin zheng on 2023/9/15.
//

#pragma once

namespace pkv {

/**
 * @brief Represents a node in a Redis cluster.
 */
class cluster_node {
public:
    /**
     * @brief Constructs a cluster node with the given address and maximum number of slots.
     * 
     * @param addr The address of the node.
     */
    cluster_node(const char* addr);

    /**
     * @brief Destroys the cluster node and frees any allocated resources.
     */
    ~cluster_node();

    cluster_node& set_id(const std::string& id);
    cluster_node& set_type(const std::string& type);
    cluster_node& set_join_time(long long join_time);
    cluster_node& set_idx(int idx);
    cluster_node& set_connected(bool connected);

    /**
     * @brief Adds the given slots to the node.
     * 
     * @param slots A vector of slot numbers to add.
     */
    void add_slots(const std::vector<size_t> &slots);

public:
    /**
     * @brief Returns the address of the node.
     * 
     * @return The address of the node.
     */
    NODISCARD const std::string& get_addr() const {
        return addr_;
    }

    NODISCARD const std::string& get_id() const {
        return id_;
    }

    NODISCARD const std::string& get_type() const {
        return type_;
    }

    NODISCARD long long get_join_time() const {
        return join_time_;
    }

    NODISCARD int get_idx() const {
        return idx_;
    }

    NODISCARD bool is_connected() const {
        return connected_;
    }

    // Get the slots in the node.
    std::vector<std::pair<size_t, size_t>> get_slots() const;

private:
    std::string id_;
    std::string addr_; /**< The address of the node. */
    std::string type_ = "myself,master";
    long long join_time_ = 0; /**< The time of joining the cluster */
    int idx_ = 0;
    bool connected_ = true;
    ACL_DLINK* slots_ = nullptr;
};

using shared_node = std::shared_ptr<cluster_node>;

} // namespace pkv
