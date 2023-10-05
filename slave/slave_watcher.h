//
// Created by shuxin zheng on 2023/9/25.
//

#pragma once
#include "common/kv_message.h"
#include "db/db_watcher.h"
#include "slave_client.h"
#include "common/message_box.h"

namespace pkv {

class slave_client;

class slave_watcher : public db_watcher {
public:
    slave_watcher() = default;
    ~slave_watcher() override = default;

public:
    static void set_box(slave_watcher& watcher, message_box* box);

public:
    void run();
    void stop();

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
    bool eof_ = false;
    acl::fiber_tbox<message_box> box_;
    std::vector<message_box*> boxes_;

    void forward_message(kv_message* message);
};

} // namespace pkv
