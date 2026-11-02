#pragma once
#include <cstddef>
#include <string>
#include <vector>

namespace pkv {
struct config_change {
    bool password = false, audit = false, replica_policy = false;
    bool replica_read_only = true;
    std::string value;
    size_t limit = 0;
};
std::string config_canonical(const std::string& name);
bool config_pattern(const std::string& pattern);
bool config_known(const std::string& name);
bool config_matches(const std::string& pattern, const std::string& name);
const std::vector<std::string>& config_names();
// Validate the complete list before publishing the prepared change.
std::string config_prepare(const std::vector<const std::string*>& pairs,
    config_change& change);
} // namespace pkv
