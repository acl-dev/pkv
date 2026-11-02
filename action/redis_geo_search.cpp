#include "stdafx.h"
#include "redis_geo.h"
#include "redis_handler.h"
#include "geo_search_parser.h"
#include "dao/paged_zset.h"
#include "common/redis_double.h"
#include "common/command_keys.h"

namespace pkv {
namespace {
void coordinates(redis_object& reply, const geo::point& point) {
    reply.set_type(REDIS_OBJ_ARRAY);
    for (const auto number : {point.longitude, point.latitude}) {
        auto& child = reply.create_child();
        child.set_string(format_redis_double(number));
        child.response_as(redis_object::response_kind::real);
    }
}
void emit(redis_object& reply, const std::vector<geo::match>& values,
    const geo::search_options& options) {
    reply.set_type(REDIS_OBJ_ARRAY);
    const auto nested = options.withdist || options.withhash ||
        options.withcoord;
    for (const auto& value : values) {
        auto& item = reply.create_child();
        if (!nested) { item.set_string(value.member); continue; }
        item.set_type(REDIS_OBJ_ARRAY);
        item.create_child().set_string(value.member);
        if (options.withdist) item.create_child().set_string(
            geo::distance_text(value.distance / options.area.conversion));
        if (options.withhash) item.create_child().set_number(
            static_cast<long long>(value.score));
        if (options.withcoord) coordinates(item.create_child(), value.position);
    }
}
}
bool redis_geo::search(const char* command, redis_coder& result) {
    const bool store = !strcasecmp(command, "GEOSEARCHSTORE");
    const auto& source = *obj_.argument(store ? 2 : 1);
    const auto mode = radius_mode(command);
    size_t target = 0;
    std::vector<const std::string*> args;
    if (mode != geo_radius_mode::none &&
        (!radius_readonly(mode) || check_cluster_mode(true))) {
        for (size_t i = 0; i < obj_.size(); ++i)
            args.push_back(obj_.argument(i));
        if (!radius_readonly(mode)) target = radius_destination(args);
    }
    const auto* destination = store ? obj_.argument(1) :
        target ? obj_.argument(target) : nullptr;
    std::vector<std::string> keys{source};
    if (destination) keys.push_back(*destination);
    if (check_cluster_mode(true)) {
        if (mode != geo_radius_mode::none) {
            command_keys(args); keys.clear();
            for (const auto* key : args) keys.push_back(*key);
        }
        if (route_keys(keys, result)) return true;
    }
    geo_search_parser parser(obj_, command);
    dao::paged_zset data;
    geo::search_options options;
    std::vector<geo::match> values;
    long long stored;
    const auto ok = data.geo_search(handler_.get_db(), source, destination,
        [&](zset_store& input, geo::search_options& parsed) {
        return parser.parse(input, parsed);
    }, options, values, stored);
    auto& reply = result.create_object();
    if (!ok) reply.set_error(parser.error().empty() ? data.error() :
        parser.error());
    else if (destination) reply.set_number(stored);
    else emit(reply, values, options);
    return true;
}
} // namespace pkv
