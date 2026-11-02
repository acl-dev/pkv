#pragma once
#include <cstddef>

namespace pkv {
struct expiry_progress {
    bool ok = true;
    bool supported = true;
    bool more = false;
    size_t checked = 0;
    size_t removed = 0;
};
} // namespace pkv
