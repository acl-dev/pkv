#pragma once
#include "slave_watcher.h"

namespace pkv {
struct slave_watcher::prepared_move : family_publication {
    slave_watcher& owner_;
    exec_entry entry_;
    std::vector<exec_entry> converted_;
    std::unique_ptr<publication> item_;
    size_t bytes_ = 0, records_ = 0, charge_ = 0, count_ = 0, prefix_ = 0;
    unsigned long long generation_ = 0, epoch_ = 0;
    bool grouped_ = false, reserved_ = false;
    prepared_move(slave_watcher& owner, exec_entry entry)
    : owner_(owner), entry_(std::move(entry)) {}
    ~prepared_move() override;
    void publish() noexcept override;
};
} // namespace pkv
