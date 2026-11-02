#include "stdafx.h"
#include "redis_geo.h"
#include "redis_handler.h"
#include "dao/paged_zset.h"
#include "common/redis_geo.h"
#include "common/geo_radius.h"
#include "common/redis_double.h"

namespace pkv {
namespace {
void real(redis_object& reply, double value) {
    reply.set_string(format_redis_double(value));
    reply.response_as(redis_object::response_kind::real);
}
}
bool redis_geo::exec(const char* command, redis_coder& result) {
    if (!strcasecmp(command, "GEOSEARCH") ||
        !strcasecmp(command, "GEOSEARCHSTORE") ||
        radius_mode(command) != geo_radius_mode::none)
        return search(command, result);
    const bool adding = !strcasecmp(command, "GEOADD");
    if (!adding && strcasecmp(command, "GEOPOS") &&
        strcasecmp(command, "GEOHASH") && strcasecmp(command, "GEODIST"))
        return false;
    // Cluster admission precedes argument parsing, as in the reference.
    if (check_cluster_mode(true) && route_keys(command, result)) return true;
    return adding ? add(result) : read(command, result);
}
bool redis_geo::add(redis_coder& result) {
    const auto error = [&](const std::string& text) {
        result.create_object().set_error(text); return true;
    };
    dao::zadd_options options;
    size_t first = 2;
    for (; first < obj_.size(); ++first) {
        const auto* name = obj_.argument(first)->c_str();
        if (!strcasecmp(name, "NX")) options.nx = true;
        else if (!strcasecmp(name, "XX")) options.xx = true;
        else if (!strcasecmp(name, "CH")) options.ch = true;
        else break;
    }
    if (options.nx && options.xx) return error("ERR syntax error");
    if (first == obj_.size() || (obj_.size() - first) % 3)
        return error("ERR syntax error");
    std::vector<zset_member> values;
    for (size_t i = first; i < obj_.size(); i += 3) {
        geo::point value;
        if (!parse_redis_double(*obj_.argument(i), value.longitude) ||
            !parse_redis_double(*obj_.argument(i + 1), value.latitude))
            return error("ERR value is not a valid float");
        if (!geo::valid(value)) return error(geo::coordinate_error(value));
        values.push_back({*obj_.argument(i + 2),
            static_cast<double>(geo::encode(value))});
    }
    dao::paged_zset data;
    dao::zadd_result out;
    auto& reply = result.create_object();
    if (!data.add(handler_.get_db(), *obj_.argument(1), values, options, out))
        reply.set_error(data.error());
    else reply.set_number(options.ch ? out.changed : out.added);
    return true;
}
bool redis_geo::read(const char* command, redis_coder& result) {
    const bool distance = !strcasecmp(command, "GEODIST");
    const bool position = !strcasecmp(command, "GEOPOS");
    const auto error = [&](const char* text) {
        result.create_object().set_error(text); return true;
    };
    double unit = 1;
    if (distance) {
        if (obj_.size() > 5) {
            return error("ERR syntax error");
        }
        if (obj_.size() == 5 && (unit = geo::unit(*obj_.argument(4))) < 0) {
            return error("ERR unsupported unit provided. "
                "please use M, KM, FT, MI");
        }
    }
    auto& reply = result.create_object();
    dao::paged_zset data;
    std::vector<std::string> members;
    for (size_t i = 2; i < (distance ? 4 : obj_.size()); ++i)
        members.push_back(*obj_.argument(i));
    std::vector<zset_score> scores;
    if (!data.scores(handler_.get_db(), *obj_.argument(1), members, scores)) {
        reply.set_error(data.error()); return true;
    }
    if (distance) {
        if (!scores[0].found || !scores[1].found) reply.set_null();
        else reply.set_string(geo::distance_text(geo::distance(
            geo::decode(scores[0].score),
            geo::decode(scores[1].score)) / unit));
        return true;
    }
    reply.set_type(REDIS_OBJ_ARRAY);
    for (const auto& score : scores) {
        auto& child = reply.create_child();
        if (!score.found) {
            child.set_type(position ? REDIS_OBJ_NULL_ARRAY :
                REDIS_OBJ_NULL_STRING);
            continue;
        }
        const auto value = geo::decode(score.score);
        if (!position) child.set_string(geo::hash(value));
        else {
            child.set_type(REDIS_OBJ_ARRAY);
            real(child.create_child(), value.longitude);
            real(child.create_child(), value.latitude);
        }
    }
    return true;
}
} // namespace pkv
