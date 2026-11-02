#pragma once
#include <cstdint>
#include <string>

namespace pkv {
// Instantiate the range allocation in this target, rather than delegating
// copies to an exported standard-library copy constructor.
inline std::string copy_client_string(const std::string& value) {
    return std::string(value.begin(), value.end());
}
// Requested string storage, excluding inline bytes and allocator rounding.
inline size_t string_heap_bytes(const std::string& value) {
    if (!value.capacity()) return 0;
    const auto data = reinterpret_cast<uintptr_t>(value.data());
    const auto object = reinterpret_cast<uintptr_t>(&value);
    return data >= object && data - object < sizeof(value) ?
        0 : value.capacity() + 1;
}
} // namespace pkv
