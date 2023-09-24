//
// Created by zsx on 2023/9/24.
//

#pragma once
#include "db/db_watcher.h"

namespace pkv {

class sync_watcher : public db_watcher {
public:
    sync_watcher();
    ~sync_watcher() override;

protected:
    // @override
    void on_set(const std::string& key, const std::string& value, bool ok) override;

    // @override
    void on_get(const std::string& key, const std::string& value, bool ok) override;

    // @override
    void on_del(const std::string& key, bool ok) override;

private:
    acl::fiber_tbox2<bool> box_;
};

} // namespace pkv
