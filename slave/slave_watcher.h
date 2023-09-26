//
// Created by shuxin ¡¡¡¡zheng on 2023/9/25.
//

#pragma once
#include "db/db_watcher.h"

namespace pkv {

class slave_client;

class slave_watcher : public db_watcher {
public:
    slave_watcher();
    ~slave_watcher() override;

protected:
    // @override
    void on_set(const std::string& key, const std::string& value, bool ok) override;

    // @override
    void on_get(const std::string& key, const std::string& value, bool ok) override;

    // @override
    void on_del(const std::string& key, bool ok) override;

private:
    //acl::fiber_tbox<> box_;
};

} // namespace pkv
