#pragma once
#include <cstddef>

namespace pkv {
struct redis_memory {
    size_t bytes = 0, spare = 0, owned = 0;
    void add(const redis_memory& other) {
        bytes += other.bytes; spare += other.spare; owned += other.owned;
    }
};
struct redis_input_memory {
    redis_memory complete, partial;
};
} // namespace pkv
