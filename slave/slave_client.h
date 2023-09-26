//
// Created by shuxin ¡¡¡¡zheng on 2023/9/26.
//

#pragma once

namespace pkv {

class slave_client {
public:
    slave_client(acl::shared_stream conn);
    ~slave_client();

    void run();

private:
    acl::shared_stream conn_;
};

} // namespace pkv
