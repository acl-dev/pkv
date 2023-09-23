//
// Created by shuxin zheng on 2023/9/8.
//

#include "stdafx.h"
<<<<<<< HEAD
=======
#include "gson/nodes_struct.h"
#include "gson/nodes_struct.gson.h"
#include <acl-lib/acl_cpp/serialize/serialize.hpp>
>>>>>>> master
#include "cluster_service.h"

namespace pkv {

<<<<<<< HEAD
cluster_service::~cluster_service() {
    delete slots_;
}

bool cluster_service::bind(const char *addr, int max_slots) {
    if (!server_.open(addr)) {
        logger_error("Bind %s error %s", addr, acl::last_serror());
        return false;
    }
    logger("Bind %s ok", addr);

    assert(max_slots > 0);

    slots_ = new acl::bitmap(max_slots);
    return true;
}

void cluster_service::add_slots(const std::vector<int> &slots) {
    for (auto& slot : slots) {
        slots_->bit_set((size_t) slot);
    }
}

} // namespace pkv
=======
void cluster_service::init(const char* save_path) {
    if (save_path && *save_path) {
        save_path_ = save_path;
        acl_make_dirs(save_path_.c_str(), 0755);
        save_path_ += "/nodes.conf";
    }
}

bool cluster_service::bind(const char *addr, size_t max_slots) {
    if (!server_.open(addr)) {
        logger_error("Bind %s error %s", addr, acl::last_serror());
        //return false;
    }

    logger("Bind %s ok", addr);

    if (max_slots > 0) {
        max_slots_ = max_slots;
    }

    slots_.clear();
    for (size_t i = 0; i < max_slots_; i++) {
        slots_.push_back(nullptr);
    }
    return true;
}

bool cluster_service::add_slots(const std::string &addr,
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

bool cluster_service::add_node(const acl::redis_node& node) {
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

shared_node cluster_service::get_node(const std::string &addr) const {
    auto it = nodes_.find(addr);
    return it == nodes_.end() ? nullptr : it->second;
}

bool cluster_service::update_join_time(const std::string &addr) const {
    auto node = get_node(addr);
    if (node == nullptr) {
        logger_error("Not found the node with addr=%s", addr.c_str());
        return false;
    }

    long long now = get_stamp();
    node->set_join_time(now);
    return true;
}

shared_node cluster_service::get_node(const char *key, size_t& slot) {
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

size_t cluster_service::hash_slot(const char *key) const {
    unsigned hash = acl_hash_crc32(key, strlen(key));
    return hash % max_slots_;
}

long long cluster_service::get_stamp() {
    struct timeval now = { 0, 0 };
    gettimeofday(&now, nullptr);
    return now.tv_sec * 1000 + now.tv_usec / 1000;
}

void cluster_service::show_null_slots() const {
    for (size_t i = 0; i < slots_.size(); i++) {
        if (slots_[i] == nullptr) {
            printf(">>>%s: null slot=%zd\r\n", __func__, i);
        }
    }
}

bool cluster_service::save_nodes() const {
    if (save_path_.empty()) {
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
    if (!fp.open_write(save_path_.c_str())) {
        logger_error("Open %s error %s", save_path_.c_str(), acl::last_serror());
        return false;
    }

    if (fp.write(buf) == -1) {
        logger_error("Write to %s error %s", save_path_.c_str(),
            acl::last_serror());
        return false;
    }

    return true;
}

bool cluster_service::load_nodes() {
    if (save_path_.empty()) {
        logger_error("save_path_ empty, call init first!");
        return false;
    }

    acl::string buf;
    if (!acl::ifstream::load(save_path_.c_str(), buf) || buf.empty()) {
        logger("Nothing in %s", save_path_.c_str());
        return true;
    }

    acl::json json(buf);
    if (!json.finish()) {
        logger_error("Invalid json=%s, file=%s", buf.c_str(), save_path_.c_str());
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

void cluster_service::add_node(const struct pkv_node& node) {
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
>>>>>>> master
