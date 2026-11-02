#include "geo_radius.h"
#include <strings.h>

namespace pkv {
geo_radius_mode radius_mode(const char* command) {
    if (!strcasecmp(command, "GEORADIUS")) return geo_radius_mode::coordinates;
    if (!strcasecmp(command, "GEORADIUSBYMEMBER"))
        return geo_radius_mode::member;
    if (!strcasecmp(command, "GEORADIUS_RO"))
        return geo_radius_mode::coordinates_ro;
    if (!strcasecmp(command, "GEORADIUSBYMEMBER_RO"))
        return geo_radius_mode::member_ro;
    return geo_radius_mode::none;
}
bool radius_readonly(geo_radius_mode mode) {
    return mode == geo_radius_mode::coordinates_ro ||
        mode == geo_radius_mode::member_ro;
}
bool radius_member(geo_radius_mode mode) {
    return mode == geo_radius_mode::member ||
        mode == geo_radius_mode::member_ro;
}
size_t radius_destination(const std::vector<const std::string*>& args) {
    if (args.empty()) return 0;
    const auto mode = radius_mode(args.front()->c_str());
    if (mode == geo_radius_mode::none || radius_readonly(mode)) return 0;
    size_t target = 0;
    for (size_t i = radius_member(mode) ? 5 : 6; i + 1 < args.size(); ++i) {
        const auto* option = args[i]->c_str();
        if (!strcasecmp(option, "STORE") || !strcasecmp(option, "STOREDIST"))
            target = ++i;
    }
    return target;
}
} // namespace pkv
