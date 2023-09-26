#pragma once

namespace pkv {

struct slot_range {
    size_t begin;
    size_t end;
    slot_range() {
        begin = 0;
        end = 0;
    }
};

struct pkv_node {
    std::string id;
    std::string addr;
    int mana_port;
    std::string type;
    int idx;
    long long join_time;
    bool connected;
    std::vector<slot_range> slots;
};

struct pkv_nodes {
    std::vector<struct pkv_node> nodes;
};

} // namespace pkv
