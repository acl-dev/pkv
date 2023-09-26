//
// Created by shuxin ¡¡¡¡zheng on 2023/9/25.
//

#pragma once
#include "db/db_watcher.h"

namespace pkv {

class slave_watcher : public db_watcher {
public:
    slave_watcher();
    ~slave_watcher() override;

protected:

};

} // namespace pkv
