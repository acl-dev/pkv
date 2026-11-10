#include "stdafx.h"
#include "geo_search_parser.h"
#include "common/redis_double.h"
#include "common/redis_integer.h"

namespace pkv {
geo_search_parser::geo_search_parser(const redis_object& object,
    const char* command) : object_(object),
    store_(!strcasecmp(command, "GEOSEARCHSTORE")),
    radius_(radius_mode(command)) {}
bool geo_search_parser::coordinates(size_t first, geo::point& out) {
    if (!parse_redis_double(*object_.argument(first), out.longitude) ||
        !parse_redis_double(*object_.argument(first + 1), out.latitude))
        return fail("ERR value is not a valid float");
    return geo::valid(out) || fail(geo::coordinate_error(out));
}
bool geo_search_parser::dimensions(size_t first, bool box, geo::shape& out) {
    double width, height = 0;
    if (!parse_redis_double(*object_.argument(first), width))
        return fail(box ? "ERR need numeric width" : "ERR need numeric radius");
    if (box && !parse_redis_double(*object_.argument(first + 1), height))
        return fail("ERR need numeric height");
    if (width < 0 || height < 0) return fail(box ?
        "ERR height or width cannot be negative" :
        "ERR radius cannot be negative");
    const auto conversion = geo::unit(*object_.argument(first + (box ? 2 : 1)));
    if (conversion < 0) return fail("ERR unsupported unit provided. "
        "please use M, KM, FT, MI");
    out.box = box; out.conversion = conversion;
    out.radius = width; out.width = width; out.height = height;
    return true;
}
bool geo_search_parser::member(const geo::search_source& source, const std::string& name,
    geo::point& out) {
    if (!source.exists) return true;
    zset_score score;
    if (!source.get(name, score)) return false;
    if (!score.found) return fail("ERR could not decode requested zset member");
    out = geo::decode(score.score); return true;
}
bool geo_search_parser::parse(const geo::search_source& source, geo::search_options& out) {
    bool from_member = false, from_coordinates = false;
    bool by_radius = false, by_box = false;
    const bool legacy = radius_ != geo_radius_mode::none;
    size_t first = store_ ? 3 : 2;
    if (legacy) {
        first = radius_member(radius_) ? 5 : 6;
        if (!radius_member(radius_)) {
            if (!coordinates(2, out.area.center) ||
                !dimensions(4, false, out.area)) return false;
        } else if (source.exists) {
            if (!member(source, *object_.argument(2), out.area.center) ||
                !dimensions(3, false, out.area)) return false;
        }
    }
    for (size_t i = first; i < object_.size(); ++i) {
        const auto* name = object_.argument(i)->c_str();
        const auto remaining = object_.size() - i - 1;
        const auto is = [&](const char* value) {
            return !strcasecmp(name, value);
        };
        if (is("WITHDIST")) out.withdist = true;
        else if (is("WITHHASH")) out.withhash = true;
        else if (is("WITHCOORD")) out.withcoord = true;
        else if (is("ANY")) out.any = true;
        else if (is("ASC")) out.order = 1;
        else if (is("DESC")) out.order = -1;
        else if (is("STOREDIST") && store_) out.storedist = true;
        else if (legacy && !radius_readonly(radius_) && remaining >= 1 &&
            (is("STORE") || is("STOREDIST"))) {
            destination_ = object_.argument(++i);
            out.storedist = is("STOREDIST");
        }
        else if (is("COUNT") && remaining >= 1) {
            if (!parse_redis_integer(*object_.argument(++i), out.count))
                return fail("ERR value is not an integer or out of range");
            if (out.count <= 0) return fail("ERR COUNT must be > 0");
        } else if (is("FROMMEMBER") && remaining >= 1 && !legacy &&
            !from_coordinates) {
            if (!member(source, *object_.argument(++i), out.area.center))
                return false;
            from_member = true;
        } else if (is("FROMLONLAT") && remaining >= 2 && !legacy &&
            !from_member) {
            if (!coordinates(i + 1, out.area.center)) return false;
            from_coordinates = true; i += 2;
        } else if (is("BYRADIUS") && remaining >= 2 && !legacy && !by_box) {
            if (!dimensions(i + 1, false, out.area)) return false;
            by_radius = true; i += 2;
        } else if (is("BYBOX") && remaining >= 3 && !legacy && !by_radius) {
            if (!dimensions(i + 1, true, out.area)) return false;
            by_box = true; i += 3;
        } else return fail("ERR syntax error");
    }
    if ((store_ || destination_) &&
        (out.withdist || out.withhash || out.withcoord))
        return fail(std::string("ERR ") + (store_ ? "GEOSEARCHSTORE" :
            "STORE option in GEORADIUS") +
            " is not compatible with WITHDIST, WITHHASH and WITHCOORD options");
    const auto& command = *object_.argument(0);
    if (!legacy && !from_member && !from_coordinates) return fail(
        "ERR exactly one of "
        "FROMMEMBER or FROMLONLAT can be specified for " + command);
    if (!legacy && !by_radius && !by_box) return fail(
        "ERR exactly one of BYRADIUS and "
        "BYBOX can be specified for " + command);
    if (out.any && !out.count)
        return fail("ERR the ANY argument requires COUNT argument");
    if (out.count && !out.order && !out.any) out.order = 1;
    return true;
}
} // namespace pkv
