#pragma once
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace pkv {
// Conservatively identify one referenced logical DB. Invalid SELECT/COPY
// operands retain ordinary runtime-error handling. No command is executed here.
bool transaction_database(
    const std::vector<std::vector<std::string>>& commands, size_t initial,
    size_t count, bool cluster, size_t& selected,
    std::vector<const std::string*>& legacy_writers,
    const std::function<bool(const std::vector<std::string>&)>& allowed = {});
} // namespace pkv
