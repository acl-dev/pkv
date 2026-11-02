#include "stdafx.h"
#include "key_count.h"

namespace pkv {
db::count_status db::key_count(uint64_t& out) {
    out = 0;
    execution_guard admission(true);
    uint64_t count = 0;
    const auto status = dbkey_count(count);
    if (status == count_status::ok) out = count;
    return status;
}
} // namespace pkv
