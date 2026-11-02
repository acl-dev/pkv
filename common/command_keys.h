#pragma once
#include <string>
#include <vector>

namespace pkv {
// Input includes the command name. Output contains only borrowed key pointers.
// Invalid key counts produce no keys; command execution reports syntax errors.
bool command_keys(std::vector<const std::string*>& arguments);
} // namespace pkv
