#pragma once
#include "exec_batch.h"

namespace pkv {
class exec_stream {
public:
    static bool send(acl::shared_stream& connection, const exec_batch& batch,
        unsigned protocol = 3);
};
} // namespace pkv
