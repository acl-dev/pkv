#pragma once
#include <string>
#include <vector>

namespace pkv {
enum class geo_radius_mode { none, coordinates, member, coordinates_ro,
    member_ro };
geo_radius_mode radius_mode(const char* command);
bool radius_readonly(geo_radius_mode mode);
bool radius_member(geo_radius_mode mode);
// Effective target: scan optional arguments and retain the last STORE or
// STOREDIST value, skipping the consumed value itself.
size_t radius_destination(const std::vector<const std::string*>& args);
} // namespace pkv
