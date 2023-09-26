//
// Created by shuxin zheng on 2023/9/8.
//

#include "stdafx.h"
#include "gson/nodes_struct.h"
#include "gson/nodes_struct.gson.h"
#include <acl-lib/acl_cpp/serialize/serialize.hpp>
#include "cluster_manager.h"

namespace pkv {

void cluster_manager::init(size_t max_slots, bool cluster_mode, const char* save_path) {
    if (max_slots > 0) {
        max_slots_ = max_slots;
    }

    slots_.clear();
    for (size_t i = 0; i < max_slots_; i++) {
        slots_.push_back(nullptr);
    }

    if (cluster_mode && save_path && *save_path) {
        acl_make_dirs(save_path, 0755);
        save_filepath_ = save_path;
        save_filepath_ += "/nodes.conf";

        if (!cluster_manager::get_instance().load_nodes(save_filepath_.c_str())) {
            logger_error("Load nodes info failed in %s", save_filepath_.c_str());
        } else {
            logger_error("Load nodes info ok in %s", save_filepath_.c_str());
        }
    }
}

bool cluster_manager::add_slots(const std::string &addr,
      const std::vector<size_t> &slots) {
    if (slots.size() > slots_.size()) {
        logger_error("Slots overflow, capacity=%zd, adding=%zd",
                slots_.size(), slots.size());
        return false;
    }

    shared_node node;
    auto it = nodes_.find(addr);
    if (it == nodes_.end()) {
        node = std::make_shared<cluster_node>(addr.c_str());
        nodes_[addr] = node;
    } else {
        node = it->second;
    }

    node->add_slots(slots);

    size_t max = 0;
    for (auto slot : slots) {
        slots_[slot] = node;
        if (slot > max) max = slot;
    }

    //show_null_slots();
    return true;
}

bool cluster_manager::add_node(const acl::redis_node& node) {
    auto addr = node.get_addr();
    if (addr == nullptr || *addr == 0) {
        logger_error("Addr null");
        return false;
    }

    auto id = node.get_id();
    if (id == nullptr || *id == 0) {
        logger_error("ID empty");
        return false;
    }

    auto type = node.get_type();
    if (type == nullptr || *type == 0) {
        logger_error("TYPE empty");
        return false;
    }

    long long join_time = get_stamp();
    int idx = 0;
    bool connected = node.is_connected();

    shared_node snode;
    auto it = nodes_.find(addr);
    if (it == nodes_.end()) {
        snode = std::make_shared<cluster_node>(addr);
        nodes_[addr] = snode;
    } else {
        snode = it->second;
    }

    (*snode).set_id(id).set_type(type).set_join_time(join_time).set_idx(idx)
        .set_connected(connected);

    auto slots_v = node.get_slots();
    std::vector<size_t> slots;
    for (auto itv : slots_v) {
        for (size_t slot = itv.first; slot <= itv.second; ++slot) {
            slots.push_back(slot);
        }
    }

    snode->add_slots(slots);

    for (auto slot : slots) {
        slots_[slot] = snode;
    }

    //show_null_slots();
    return true;
}

shared_node cluster_manager::get_node(const std::string &addr) const {
    auto it = nodes_.find(addr);
    return it == nodes_.end() ? nullptr : it->second;
}

bool cluster_manager::update_join_time(const std::string &addr) const {
    auto node = get_node(addr);
    if (node == nullptr) {
        logger_error("Not found the node with addr=%s", addr.c_str());
        return false;
    }

    long long now = get_stamp();
    node->set_join_time(now);
    return true;
}

shared_node cluster_manager::get_node(const char *key, size_t& slot) {
    slot = hash_slot(key);
    auto node = slots_[slot];

#ifdef DEBUG_CLUSTER
    if (node == nullptr) {
        show_null_slots();
        assert(node);
    }
#endif
    return node;
}

size_t cluster_manager::hash_slot(const char *key) const {
    unsigned hash = acl_hash_crc32(key, strlen(key));
    return hash % max_slots_;
}

long long cluster_manager::get_stamp() {
    struct timeval now = { 0, 0 };
    gettimeofday(&now, nullptr);
    return now.tv_sec * 1000 + now.tv_usec / 1000;
}

void cluster_manager::show_null_slots() const {
    for (size_t i = 0; i < slots_.size(); i++) {
        if (slots_[i] == nullptr) {
            printf(">>>%s: null slot=%zd\r\n", __func__, i);
        }
    }
}

bool cluster_manager::save_nodes() const {
    if (save_filepath_.empty()) {
        logger_error("save_path_ empty, call init first!");
        return false;
    }

    struct pkv_nodes nodes;

    for (auto& item : nodes_) {
        struct pkv_node node;
        node.id = item.second->get_id();
        node.addr = item.second->get_addr();
        node.mana_port = 0;
        node.type = item.second->get_type();
        node.idx = item.second->get_idx();
        node.join_time = item.second->get_join_time();
        node.connected = item.second->is_connected();

        auto slots = item.second->get_slots();
        for (auto& it : slots) {
            struct slot_range range;
            range.begin = it.first;
            range.end = it.second;
            node.slots.emplace_back(range);
        }

        nodes.nodes.emplace_back(node);
    }

    acl::string buf;
    acl::serialize<struct pkv_nodes>(nodes, buf);

    acl::ofstream fp;
    if (!fp.open_write(save_filepath_.c_str())) {
        logger_error("Open %s error %s", save_filepath_.c_str(), acl::last_serror());
        return false;
    }

    if (fp.write(buf) == -1) {
        logger_error("Write to %s error %s", save_filepath_.c_str(),
            acl::last_serror());
        return false;
    }

    return true;
}

bool cluster_manager::load_nodes(const char* filepath) {
    acl::string buf;
    if (!acl::ifstream::load(filepath, buf) || buf.empty()) {
        logger("Nothing in %s", save_filepath_.c_str());
        return true;
    }

    acl::json json(buf);
    if (!json.finish()) {
        logger_error("Invalid json=%s, file=%s", buf.c_str(), filepath);
        return false;
    }

    struct pkv_nodes nodes;
    acl::string err;

    if (!acl::deserialize(json, nodes, &err)) {
        logger_error("deserialize error=%s", err.c_str());
        return false;
    }

    for (auto& node : nodes.nodes) {
        add_node(node);
    }

    return true;
}

void cluster_manager::add_node(const struct pkv_node& node) {
    shared_node snode;
    auto it = nodes_.find(node.addr);
    if (it == nodes_.end()) {
        snode = std::make_shared<cluster_node>(node.addr.c_str());
        nodes_[node.addr] = snode;
    } else {
        snode = it->second;
    }

    (*snode).set_id(node.id).set_type(node.type).set_join_time(node.join_time)
        .set_idx(node.idx).set_connected(node.connected);

    std::vector<size_t> slots;
    for (auto &item : node.slots) {
        for (size_t slot = item.begin; slot <= item.end; ++slot) {
            slots.push_back(slot);
        }
    }

    snode->add_slots(slots);

    for (auto slot : slots) {
        slots_[slot] = snode;
    }
}

} // namespace pkv
