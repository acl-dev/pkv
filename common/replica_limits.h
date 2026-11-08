#pragma once
#include <cstddef>

namespace pkv {
// Grouped wire records have a separate, bounded native expansion budget.
constexpr size_t replica_action_factor = 4;
constexpr size_t replica_storage_factor = 5;
constexpr size_t replica_object_padding = 256;
} // namespace pkv
