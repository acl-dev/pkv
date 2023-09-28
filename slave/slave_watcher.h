//
// Created by shuxin zheng on 2023/9/25.
//

#pragma once
#include "db/kv_message.h"
#include "db/db_watcher.h"
#include "slave_client.h"

namespace pkv {

class slave_client;

class slave_watcher : public db_watcher {
public:
    slave_watcher() = default;
    ~slave_watcher() override = default;

public:
    void run();

    void add_client(const shared_client& client);
    bool del_client(const shared_client& client);

protected:
    // @override
    bool on_set(const std::string& key, const std::string& value, bool ok) override;

    // @override
    bool on_get(const std::string& key, const std::string& value, bool ok) override;

    // @override
    bool on_del(const std::string& key, bool ok) override;

private:
    std::vector<shared_client> clients_;
    acl::fiber_tbox2<shared_message> box_;

    void forward_message(const shared_message& message);
};

} // namespace pkv
