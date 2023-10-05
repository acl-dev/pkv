//
// Created by shuxin zheng on 2023/9/26.
//

#pragma once
#include "common/message_box.h"

namespace pkv {

class slave_client : public std::enable_shared_from_this<slave_client> {
public:
    explicit slave_client(acl::shared_stream conn);
    ~slave_client();

    void run();

    void push(kv_message* message);

    acl::shared_stream& get_conn() {
        return conn_;
    }

private:
    acl::shared_stream conn_;
    message_box box_;
    bool eof_ = false;

    bool flush(std::vector<kv_message*>& messages);
};

using shared_client = std::shared_ptr<slave_client>;

} // namespace pkv
